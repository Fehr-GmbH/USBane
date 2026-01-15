/*
 * USBane - USB Module
 * 
 * Direct DWC2 USB controller access for security research
 */

#include "usb_malformed.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "soc/usb_wrap_struct.h"
#include "soc/usb_periph.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "nvs.h"

// Include USB HAL headers
#include "hal/usb_dwc_hal.h"
#include "hal/usb_dwc_ll.h"
#include "hal/usb_wrap_ll.h"

static const char *TAG = "USB_MALFORMED";
static const char *NVS_NAMESPACE = "usb_config";

// USB PHY handle
static usb_phy_handle_t phy_hdl = NULL;

// HAL context
static usb_dwc_hal_context_t hal_context;

// Cached device info
static usb_device_info_t cached_device_info = {0};

// Consecutive failure counter for auto-recovery
static int consecutive_failures = 0;
static int recovery_attempts = 0;
#define MAX_CONSECUTIVE_FAILURES 5  // Reset USB after this many failures
#define MAX_RECOVERY_ATTEMPTS 3     // Power cycle after this many failed resets

// ============================================================================
// USB Worker Task (Core 1) - Single Operation Model
// ============================================================================

// Operation types
typedef enum {
    USB_OP_NONE = 0,
    USB_OP_INIT,
    USB_OP_RESET,
    USB_OP_SEND_PACKET,
    USB_OP_GET_STATUS,
    USB_OP_GET_DEVICE_INFO,
    USB_OP_CLEAR_CACHE
} usb_op_type_t;

// Pending operation structure
typedef struct {
    usb_op_type_t type;
    void *arg1;
    void *arg2;
    esp_err_t result;
    bool bool_result;
} usb_pending_op_t;

// Synchronization primitives
static SemaphoreHandle_t usb_mutex = NULL;       // Protects access to pending_op
static SemaphoreHandle_t usb_op_ready = NULL;    // Signals worker: work available
static SemaphoreHandle_t usb_op_done = NULL;     // Signals caller: work complete
static usb_pending_op_t pending_op = {0};
static TaskHandle_t usb_worker_handle = NULL;

// ============================================================================
// NVS Configuration Storage
// ============================================================================

static esp_err_t load_usb_phy_config(usb_phy_config_t *phy_config) {
    // Initialize with default values (this ensures ALL fields are set properly)
    usb_phy_config_t default_config = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_HOST,
        .otg_speed = USB_PHY_SPEED_FULL,
    };
    
    // Copy defaults first
    memcpy(phy_config, &default_config, sizeof(usb_phy_config_t));
    
    // Try to load from NVS
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No saved config found, using defaults");
        return ESP_OK;
    }
    
    // Load config from NVS (only override the configurable fields)
    uint8_t otg_mode = USB_OTG_MODE_HOST;
    uint8_t otg_speed = USB_PHY_SPEED_FULL;
    
    nvs_get_u8(nvs_handle, "otg_mode", &otg_mode);
    nvs_get_u8(nvs_handle, "otg_speed", &otg_speed);
    
    nvs_close(nvs_handle);
    
    // Apply loaded values
    phy_config->otg_mode = otg_mode;
    phy_config->otg_speed = otg_speed;
    
    ESP_LOGI(TAG, "Loaded config from NVS: mode=%d speed=%d", otg_mode, otg_speed);
    
    return ESP_OK;
}

esp_err_t usb_save_phy_config(uint8_t otg_mode, uint8_t otg_speed) {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }
    
    nvs_set_u8(nvs_handle, "otg_mode", otg_mode);
    nvs_set_u8(nvs_handle, "otg_speed", otg_speed);
    
    ret = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "PHY config saved to NVS: mode=%d speed=%d", otg_mode, otg_speed);
    } else {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(ret));
    }
    
    return ret;
}

// Forward declarations
static esp_err_t usb_malformed_init_impl(void);
static esp_err_t usb_send_reset_impl(void);
static esp_err_t usb_send_packet_impl(const usb_packet_config_t *config);
static bool usb_is_device_connected_impl(void);
static esp_err_t usb_get_device_info_impl(usb_device_info_t *info);

// ============================================================================
// USB Worker Task (runs on Core 1)
// ============================================================================

/**
 * Worker task that processes USB operations on Core 1
 * Uses a simple ready/done semaphore handshake (no queue!)
 */
static void usb_worker_task(void *arg) {
    ESP_LOGW(TAG, "═══════════════════════════════════════════");
    ESP_LOGW(TAG, "USB WORKER TASK RUNNING on Core %d", xPortGetCoreID());
    ESP_LOGW(TAG, "═══════════════════════════════════════════");
    
    while (1) {
        // Wait for work
        if (xSemaphoreTake(usb_op_ready, portMAX_DELAY) == pdTRUE) {
            usb_op_type_t op_type = pending_op.type;
            ESP_LOGD(TAG, "Worker: processing op %d", op_type);
            
            // Execute the pending operation
            switch (op_type) {
                case USB_OP_INIT:
                    ESP_LOGI(TAG, "Worker: USB_OP_INIT");
                    pending_op.result = usb_malformed_init_impl();
                    break;
                    
                case USB_OP_RESET:
                    pending_op.result = usb_send_reset_impl();
                    break;
                    
                case USB_OP_SEND_PACKET:
                    pending_op.result = usb_send_packet_impl((const usb_packet_config_t *)pending_op.arg1);
                    break;
                    
                case USB_OP_GET_STATUS:
                    pending_op.bool_result = usb_is_device_connected_impl();
                    pending_op.result = ESP_OK;
                    break;
                    
                case USB_OP_GET_DEVICE_INFO:
                    pending_op.result = usb_get_device_info_impl((usb_device_info_t *)pending_op.arg1);
                    break;
                    
                case USB_OP_CLEAR_CACHE:
                    memset(&cached_device_info, 0, sizeof(usb_device_info_t));
                    pending_op.result = ESP_OK;
                    break;
                    
                default:
                    pending_op.result = ESP_ERR_NOT_SUPPORTED;
                    break;
            }
            
            ESP_LOGD(TAG, "Worker: op %d complete, result=%d", op_type, pending_op.result);
            
            // Signal completion
            xSemaphoreGive(usb_op_done);
        }
    }
}

/**
 * Execute a USB operation on Core 1 worker task
 */
static esp_err_t usb_execute_on_core1(usb_op_type_t type, void *arg1, void *arg2, uint32_t timeout_ms) {
    if (usb_mutex == NULL) {
        ESP_LOGE(TAG, "USB handler not started");
        return ESP_FAIL;
    }
    
    // Only one operation at a time
    if (xSemaphoreTake(usb_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire USB mutex");
        return ESP_ERR_TIMEOUT;
    }
    
    // Clear any stale completion signal (from previous timeout)
    xSemaphoreTake(usb_op_done, 0);
    
    // Set up the operation
    pending_op.type = type;
    pending_op.arg1 = arg1;
    pending_op.arg2 = arg2;
    pending_op.result = ESP_FAIL;
    pending_op.bool_result = false;
    
    ESP_LOGD(TAG, "Executing op %d on Core 1", type);
    
    // Signal worker to start
    xSemaphoreGive(usb_op_ready);
    
    // Wait for completion
    if (xSemaphoreTake(usb_op_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGE(TAG, "USB operation timeout (type=%d)", type);
        xSemaphoreGive(usb_mutex);
        return ESP_ERR_TIMEOUT;
    }
    
    esp_err_t result = pending_op.result;
    xSemaphoreGive(usb_mutex);
    return result;
}

/**
 * Start USB Worker Task on Core 1
 */
esp_err_t usb_handler_start(void) {
    ESP_LOGI(TAG, ">>> usb_handler_start() called");
    
    if (usb_worker_handle != NULL) {
        ESP_LOGW(TAG, "USB handler already started");
        return ESP_OK;
    }
    
    // Create synchronization primitives
    ESP_LOGI(TAG, "Creating semaphores...");
    usb_mutex = xSemaphoreCreateMutex();
    usb_op_ready = xSemaphoreCreateBinary();
    usb_op_done = xSemaphoreCreateBinary();
    
    if (usb_mutex == NULL || usb_op_ready == NULL || usb_op_done == NULL) {
        ESP_LOGE(TAG, "Failed to create USB semaphores");
        if (usb_mutex) vSemaphoreDelete(usb_mutex);
        if (usb_op_ready) vSemaphoreDelete(usb_op_ready);
        if (usb_op_done) vSemaphoreDelete(usb_op_done);
        usb_mutex = NULL;
        usb_op_ready = NULL;
        usb_op_done = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Semaphores created OK");
    
    // Create worker task on Core 0 (USB peripheral often needs Core 0)
    ESP_LOGI(TAG, "Creating worker task on Core 0 (USB peripheral requires Core 0)...");
    BaseType_t ret = xTaskCreatePinnedToCore(
        usb_worker_task,
        "usb_worker",
        8192,
        NULL,
        5,                          // Priority
        &usb_worker_handle,
        0                           // Core 0 (PRO_CPU) - USB peripheral
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create USB worker task (ret=%d)", ret);
        vSemaphoreDelete(usb_mutex);
        vSemaphoreDelete(usb_op_ready);
        vSemaphoreDelete(usb_op_done);
        usb_mutex = NULL;
        usb_op_ready = NULL;
        usb_op_done = NULL;
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "USB Worker task created, handle=%p", (void*)usb_worker_handle);
    
    // Give task time to start
    vTaskDelay(pdMS_TO_TICKS(50));
    
    ESP_LOGI(TAG, ">>> usb_handler_start() returning OK");
    return ESP_OK;
}

// ============================================================================
// Public API (Executes on Core 1 via Worker Task)
// ============================================================================

esp_err_t usb_malformed_init(void) {
    return usb_execute_on_core1(USB_OP_INIT, NULL, NULL, 10000);
}

esp_err_t usb_send_reset(void) {
    return usb_execute_on_core1(USB_OP_RESET, NULL, NULL, 5000);
}

esp_err_t usb_send_packet(const usb_packet_config_t *config) {
    // Use config timeout + 500ms buffer for the operation wrapper
    uint32_t timeout = config->timeout_ms + 500;
    if (timeout < 1000) timeout = 1000;  // Minimum 1 second
    return usb_execute_on_core1(USB_OP_SEND_PACKET, (void *)config, NULL, timeout);
}

bool usb_is_device_connected(void) {
    if (usb_mutex == NULL) return false;
    
    // Use worker task for bool return
    if (xSemaphoreTake(usb_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    
    // Clear any stale completion signal
    xSemaphoreTake(usb_op_done, 0);
    
    pending_op.type = USB_OP_GET_STATUS;
    pending_op.bool_result = false;
    
    xSemaphoreGive(usb_op_ready);
    
    if (xSemaphoreTake(usb_op_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        xSemaphoreGive(usb_mutex);
        return false;
    }
    
    bool result = pending_op.bool_result;
    xSemaphoreGive(usb_mutex);
    return result;
}

esp_err_t usb_get_device_info(usb_device_info_t *info) {
    // Shorter timeout (2s) to keep web UI responsive
    return usb_execute_on_core1(USB_OP_GET_DEVICE_INFO, info, NULL, 2000);
}

void usb_clear_device_info_cache(void) {
    usb_execute_on_core1(USB_OP_CLEAR_CACHE, NULL, NULL, 1000);
}

// ============================================================================
// Helper: Flush all USB FIFOs
// ============================================================================

static void usb_flush_all_fifos(void) {
    // Flush all TX and RX FIFOs to clear any stale data
    // This is critical for preventing state accumulation after many transfers
    usb_dwc_ll_grstctl_flush_nptx_fifo(hal_context.dev);
    usb_dwc_ll_grstctl_flush_ptx_fifo(hal_context.dev);
    usb_dwc_ll_grstctl_flush_rx_fifo(hal_context.dev);
    // Small delay to ensure flush completes
    vTaskDelay(pdMS_TO_TICKS(1));
}

// ============================================================================
// Internal Implementation
// ============================================================================

static esp_err_t usb_malformed_init_impl(void) {
    ESP_LOGI(TAG, "Initializing USB Host hardware (bypass mode)");
    
    // 1. Initialize USB PHY
    ESP_LOGI(TAG, "Step 1: Initializing USB PHY...");
    usb_phy_config_t phy_config;
    load_usb_phy_config(&phy_config);
    
    const char* mode_str = (phy_config.otg_mode == 0) ? "Host" : "Device";
    const char* speed_str = (phy_config.otg_speed == 1) ? "Full-Speed" : "Low-Speed";
    ESP_LOGI(TAG, "Host PHY configured: mode=%s (%d) speed=%s (%d)",
             mode_str, phy_config.otg_mode, speed_str, phy_config.otg_speed);
    
    esp_err_t ret = usb_new_phy(&phy_config, &phy_hdl);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize USB PHY: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "USB PHY initialized");
    
    // 2. Initialize HAL
    ESP_LOGI(TAG, "Step 2: Initializing USB HAL...");
    usb_dwc_hal_init(&hal_context, 0);
    ESP_LOGI(TAG, "USB HAL initialized");
    
    // 3. Configure as Host
    ESP_LOGI(TAG, "Step 3: Configuring as USB Host...");
    
    usb_dwc_hal_core_soft_reset(&hal_context);
    vTaskDelay(pdMS_TO_TICKS(10));
    
    usb_dwc_ll_gintsts_clear_intrs(hal_context.dev, 0xFFFFFFFF);
    
    ESP_LOGI(TAG, "  → Forcing Host Mode...");
    usb_dwc_ll_gusbcfg_force_host_mode(hal_context.dev);
    
    hal_context.dev->gotgctl_reg.bvalidovval = 1;
    hal_context.dev->gotgctl_reg.bvalidoven = 1;
    hal_context.dev->gotgctl_reg.avalidovval = 1;
    hal_context.dev->gotgctl_reg.avalidoven = 1;
    
    vTaskDelay(pdMS_TO_TICKS(25));
    
    ESP_LOGI(TAG, "  → Configuring FIFOs...");
    usb_dwc_ll_grxfsiz_set_fifo_size(hal_context.dev, 256);
    usb_dwc_ll_gnptxfsiz_set_fifo_size(hal_context.dev, 256, 256);
    usb_dwc_ll_hptxfsiz_set_ptx_fifo_size(hal_context.dev, 512, 256);
    
    // Flush all FIFOs to ensure clean state
    usb_flush_all_fifos();
    
    ESP_LOGI(TAG, "  → Enabling DMA mode...");
    usb_dwc_ll_gahbcfg_en_dma_mode(hal_context.dev);
    usb_dwc_ll_gahbcfg_set_hbstlen(hal_context.dev, 4);
    
    ESP_LOGI(TAG, "  → Enabling global interrupts...");
    usb_dwc_ll_gahbcfg_en_global_intr(hal_context.dev);
    
    ESP_LOGI(TAG, "  → Configuring USB_WRAP for host mode...");
    usb_wrap_ll_phy_enable_srp_sessend_override(&USB_WRAP, false);
    
    usb_wrap_pull_override_vals_t vals = {
        .dp_pu = false,
        .dp_pd = true,
        .dm_pu = false,
        .dm_pd = true
    };
    usb_wrap_ll_phy_enable_pull_override(&USB_WRAP, &vals);
    usb_wrap_ll_phy_enable_pad(&USB_WRAP, true);
    
    ESP_LOGI(TAG, "USB Host mode configured");
    
    // 4. Initialize host port
    ESP_LOGI(TAG, "Step 4: Initializing host port...");
    ESP_LOGI(TAG, "  → Enabling port power (VBUS)...");
    usb_dwc_ll_hprt_en_pwr(hal_context.dev);
    vTaskDelay(pdMS_TO_TICKS(200));
    
    if (usb_dwc_ll_hprt_get_conn_status(hal_context.dev)) {
        ESP_LOGI(TAG, "Device detected on port!");
    } else {
        ESP_LOGW(TAG, "No device detected yet");
    }
    
    ESP_LOGI(TAG, "USB Host initialization complete!");
    
    return ESP_OK;
}

static esp_err_t usb_send_reset_impl(void) {
    ESP_LOGI(TAG, "═══════════════════════════════════════");
    ESP_LOGI(TAG, "Starting USB Reset & Speed Negotiation");
    ESP_LOGI(TAG, "═══════════════════════════════════════");
    
    // Check device before reset
    if (usb_dwc_ll_hprt_get_conn_status(hal_context.dev)) {
        ESP_LOGI(TAG, "Device connected, checking current state...");
        usb_dwc_speed_t speed_before = usb_dwc_ll_hprt_get_speed(hal_context.dev);
        ESP_LOGI(TAG, "Speed before reset: %s", 
                 speed_before == USB_DWC_SPEED_FULL ? "Full-Speed" : 
                 speed_before == USB_DWC_SPEED_LOW ? "Low-Speed" : "Unknown");
    }
    
    // Assert reset signal
    ESP_LOGI(TAG, "Asserting USB RESET signal (15ms)...");
    usb_dwc_ll_hprt_set_port_reset(hal_context.dev, true);
    vTaskDelay(pdMS_TO_TICKS(15));
    
    // De-assert reset signal - device will now send its speed chirp
    ESP_LOGI(TAG, "De-asserting RESET - device will chirp its speed...");
    usb_dwc_ll_hprt_set_port_reset(hal_context.dev, false);
    vTaskDelay(pdMS_TO_TICKS(20));
    
    // Check negotiated result
    if (usb_dwc_ll_hprt_get_conn_status(hal_context.dev)) {
        ESP_LOGI(TAG, "Device responded! Checking negotiated speed...");
        usb_dwc_speed_t negotiated_speed = usb_dwc_ll_hprt_get_speed(hal_context.dev);
        
        const char* speed_name = (negotiated_speed == USB_DWC_SPEED_FULL) ? "Full-Speed (12 Mbps)" :
                                 (negotiated_speed == USB_DWC_SPEED_LOW) ? "Low-Speed (1.5 Mbps)" : 
                                 "Unknown";
        
        ESP_LOGI(TAG, "Negotiation complete!");
        ESP_LOGI(TAG, "Negotiated speed: %s", speed_name);
        ESP_LOGI(TAG, "═══════════════════════════════════════");
    } else {
        ESP_LOGW(TAG, "No device detected after reset");
        ESP_LOGI(TAG, "═══════════════════════════════════════");
    }
    
    return ESP_OK;
}

static bool usb_is_device_connected_impl(void) {
    return usb_dwc_ll_hprt_get_conn_status(hal_context.dev);
}

esp_err_t usb_malformed_get_conn_status(void) {
    return usb_is_device_connected() ? ESP_OK : ESP_FAIL;  // Uses public API which sends command
}

// ============================================================================
// MAIN SEND FUNCTION - All parameters are customizable
// ============================================================================

static esp_err_t usb_send_packet_impl(const usb_packet_config_t *config) {
    if (config == NULL) {
        ESP_LOGE(TAG, "Invalid config: NULL pointer");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Validate packet_size - 0 causes USB controller hang
    if (config->packet_size <= 0) {
        ESP_LOGE(TAG, "Invalid packet_size: 0 (would hang USB controller)");
        return ESP_ERR_INVALID_ARG;
    }
    
    const char* size_status = config->packet_size == 8 ? "(standard)" : 
                              config->packet_size < 8 ? "(TRUNCATED!)" : "(OVERSIZED!)";
    
    ESP_LOGI(TAG, "Sending USB packet:");
    ESP_LOGI(TAG, "  bmRequestType: 0x%02x", config->bmRequestType);
    ESP_LOGI(TAG, "  bRequest: 0x%02x", config->bRequest);
    ESP_LOGI(TAG, "  wValue: 0x%04x", config->wValue);
    ESP_LOGI(TAG, "  wIndex: 0x%04x", config->wIndex);
    ESP_LOGI(TAG, "  wLength: %d (0x%04x)", config->wLength, config->wLength);
    ESP_LOGI(TAG, "  Packet size: %d bytes %s", config->packet_size, size_status);
    
    // Build packet buffer
    static uint8_t packet_buffer[USB_MAX_PACKET_SIZE] __attribute__((aligned(4)));
    memset(packet_buffer, 0, sizeof(packet_buffer));
    
    // Build SETUP packet - only up to packet_size bytes
    // This allows truncated packets (< USB_SETUP_PACKET_SIZE bytes) for attack testing
    int bytes_to_build = config->packet_size < USB_SETUP_PACKET_SIZE ? config->packet_size : USB_SETUP_PACKET_SIZE;
    
    if (bytes_to_build >= 1) packet_buffer[0] = config->bmRequestType;
    if (bytes_to_build >= 2) packet_buffer[1] = config->bRequest;
    if (bytes_to_build >= 3) packet_buffer[2] = config->wValue & 0xFF;
    if (bytes_to_build >= 4) packet_buffer[3] = (config->wValue >> 8) & 0xFF;
    if (bytes_to_build >= 5) packet_buffer[4] = config->wIndex & 0xFF;
    if (bytes_to_build >= 6) packet_buffer[5] = (config->wIndex >> 8) & 0xFF;
    if (bytes_to_build >= 7) packet_buffer[6] = config->wLength & 0xFF;
    if (bytes_to_build >= 8) packet_buffer[7] = (config->wLength >> 8) & 0xFF;
    
    // Add extra data for oversized packets
    if (config->packet_size > USB_SETUP_PACKET_SIZE) {
        size_t extra_bytes = config->packet_size - USB_SETUP_PACKET_SIZE;
        if (extra_bytes > sizeof(config->extra_data)) {
            extra_bytes = sizeof(config->extra_data);
        }
        memcpy(&packet_buffer[USB_SETUP_PACKET_SIZE], config->extra_data, extra_bytes);
    }
    
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, packet_buffer, config->packet_size, ESP_LOG_INFO);
    
    // Configure USB host channel
    const int chan_num = 0;
    volatile usb_dwc_host_chan_regs_t *chan_regs = &hal_context.dev->host_chans[chan_num];
    
    // IMPORTANT: Fully reset channel state before each transfer
    // This prevents state accumulation that causes failures after many requests
    
    // 1. Disable channel if active and wait for it to fully halt
    if (usb_dwc_ll_hcchar_chan_is_enabled(chan_regs)) {
        usb_dwc_ll_hcchar_disable_chan(chan_regs);
        // Wait for channel to actually halt (up to 10ms)
        for (int i = 0; i < 10; i++) {
        vTaskDelay(pdMS_TO_TICKS(1));
            if (!usb_dwc_ll_hcchar_chan_is_enabled(chan_regs)) break;
        }
    }
    
    // 2. Clear ALL pending interrupts (critical for preventing state buildup)
    usb_dwc_ll_hcint_read_and_clear_intrs(chan_regs);
    
    // 3. Flush RX FIFO before every transfer (prevents stale data issues)
    usb_dwc_ll_grstctl_flush_rx_fifo(hal_context.dev);
    
    // 4. Re-initialize transfer size register
    usb_dwc_ll_hctsiz_init(chan_regs);
    
    // Configure channel
    usb_dwc_ll_hcchar_set_ep_num(chan_regs, config->endpoint);
    usb_dwc_ll_hcchar_set_dev_addr(chan_regs, config->device_addr);
    usb_dwc_ll_hcchar_set_ep_type(chan_regs, USB_DWC_XFER_TYPE_CTRL);
    usb_dwc_ll_hcchar_set_mps(chan_regs, config->max_packet_size);
    usb_dwc_ll_hcchar_set_dir(chan_regs, 0);  // OUT for SETUP
    
    // Configure transfer size
    usb_dwc_ll_hctsiz_init(chan_regs);
    chan_regs->hctsiz_reg.xfersize = config->packet_size;
    chan_regs->hctsiz_reg.pktcnt = (config->packet_size + config->max_packet_size - 1) / config->max_packet_size;
    chan_regs->hctsiz_reg.pid = 3;  // SETUP PID (MDATA)
    
    // Setup DMA - simple direct buffer (baseline approach that works)
    static uint8_t dma_buffer[256] __attribute__((aligned(4)));
    memcpy(dma_buffer, packet_buffer, config->packet_size);
    chan_regs->hcdma_reg.dmaaddr = (uint32_t)dma_buffer;
    
    // Enable channel to send
    usb_dwc_ll_hcchar_enable_chan(chan_regs);
    
    ESP_LOGI(TAG, "SETUP packet sent");
    
    // Wait for transmission (simple delay - baseline approach)
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // Check direction: bit 7 of bmRequestType (0=Host-to-Device, 1=Device-to-Host)
    bool is_host_to_device = (config->bmRequestType & 0x80) == 0;
    
    if (is_host_to_device && config->wLength > 0) {
        // DATA OUT stage (Host-to-Device)
        return usb_write_data(config->extra_data, config->wLength, config->timeout_ms);
    }
    else if (!is_host_to_device && config->expect_response && config->response_buffer != NULL) {
        // DATA IN stage (Device-to-Host)
        return usb_read_response(config->response_buffer, 
                                config->response_buffer_size,
                                config->timeout_ms,
                                config->bytes_received,
                                config->max_nak_retries);
    }
    
    return ESP_OK;
}

/**
 * Write data to device (DATA OUT stage)
 */
esp_err_t usb_write_data(const uint8_t *data, size_t length, uint32_t timeout_ms) {
    const int chan_num = 0;
    volatile usb_dwc_host_chan_regs_t *chan_regs = &hal_context.dev->host_chans[chan_num];
    
    // Determine actual data available (limited by extra_data buffer size)
    size_t available_data = (length > USB_MAX_EXTRA_DATA) ? USB_MAX_EXTRA_DATA : length;
    size_t num_chunks = (length + USB_DMA_CHUNK_SIZE - 1) / USB_DMA_CHUNK_SIZE;  // Round up
    
    ESP_LOGI(TAG, "DATA OUT: %zu bytes total (%zu chunks of %d bytes, %zu from buffer + %zu zeros)", 
             length, num_chunks, USB_DMA_CHUNK_SIZE, available_data, length - available_data);
    
    size_t total_sent = 0;
    uint8_t current_pid = 2;  // Start with DATA1
    
    // Send in chunks (max packet size for EP0)
    static uint8_t dma_buffer[USB_DMA_CHUNK_SIZE] __attribute__((aligned(4)));
    
    while (total_sent < length) {
        // Halt channel if active
        if (usb_dwc_ll_hcchar_chan_is_enabled(chan_regs)) {
            usb_dwc_ll_hcchar_disable_chan(chan_regs);
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        
        // Clear any previous interrupts
        usb_dwc_ll_hcint_read_and_clear_intrs(chan_regs);
        
        // Configure for DATA OUT
        usb_dwc_ll_hcchar_set_dir(chan_regs, 0);  // OUT direction
        
        // Calculate chunk size
        size_t remaining = length - total_sent;
        size_t chunk_size = (remaining > USB_DMA_CHUNK_SIZE) ? USB_DMA_CHUNK_SIZE : remaining;
        
        // Prepare data for this chunk
        memset(dma_buffer, 0, sizeof(dma_buffer));
        if (total_sent < available_data) {
            // We have real data available
            size_t copy_size = (total_sent + chunk_size <= available_data) ? chunk_size : (available_data - total_sent);
            memcpy(dma_buffer, data + total_sent, copy_size);
            // Rest stays zeros if chunk_size > copy_size
        }
        // else: send zeros for data beyond available buffer
        
        if (total_sent == 0) {
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, dma_buffer, chunk_size, ESP_LOG_INFO);  // Log first packet only
        }
        
        // Configure transfer size for this chunk
        usb_dwc_ll_hctsiz_init(chan_regs);
        chan_regs->hctsiz_reg.xfersize = chunk_size;
        chan_regs->hctsiz_reg.pktcnt = 1;  // One packet per chunk
        chan_regs->hctsiz_reg.pid = current_pid;
        
        // Set DMA address
        chan_regs->hcdma_reg.dmaaddr = (uint32_t)dma_buffer;
        
        // Enable channel to send DATA OUT
        usb_dwc_ll_hcchar_enable_chan(chan_regs);
        
        // Wait for completion of this chunk
        uint32_t start_time = xTaskGetTickCount();
        uint32_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
        bool chunk_success = false;
        
        while ((xTaskGetTickCount() - start_time) < timeout_ticks) {
            uint32_t hcint = usb_dwc_ll_hcint_read_and_clear_intrs(chan_regs);
            
            if (hcint & USB_DWC_LL_INTR_CHAN_CHHLTD) {
                if (hcint & USB_DWC_LL_INTR_CHAN_XFERCOMPL) {
                    chunk_success = true;
                    break;
                }
                if (hcint & USB_DWC_LL_INTR_CHAN_STALL) {
                    ESP_LOGW(TAG, "Device STALLED DATA OUT at %zu bytes", total_sent);
                    return ESP_ERR_INVALID_RESPONSE;
                }
                ESP_LOGW(TAG, "Error during DATA OUT at %zu bytes (hcint=0x%lx)", total_sent, hcint);
                return ESP_FAIL;
            }
    
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        
        if (!chunk_success) {
            ESP_LOGW(TAG, "Timeout sending DATA OUT chunk at %zu bytes", total_sent);
            return ESP_ERR_TIMEOUT;
        }
        
        total_sent += chunk_size;
        
        // Toggle PID for next packet (DATA1 <-> DATA0)
        current_pid = (current_pid == 2) ? 0 : 2;
    }
    
    ESP_LOGI(TAG, "DATA OUT sent successfully: %zu bytes (%zu from buffer, %zu zeros)", 
             total_sent, available_data, total_sent - available_data);
    return ESP_OK;
}

/**
 * Read response from device (DATA IN stage)
 * Simple baseline approach - direct buffer, no QTD
 */
esp_err_t usb_read_response(uint8_t *buffer, size_t max_len, 
                            uint32_t timeout_ms, size_t *bytes_read,
                            int max_nak_retries) {
    ESP_LOGI(TAG, "Waiting for device response (timeout: %ldms)", timeout_ms);
    
    const int chan_num = 0;
    volatile usb_dwc_host_chan_regs_t *chan_regs = &hal_context.dev->host_chans[chan_num];
    uint32_t start_time = xTaskGetTickCount();
    uint32_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    
    // IMPORTANT: Ensure channel is fully stopped from SETUP before starting DATA IN
    // This prevents state carryover issues
    if (usb_dwc_ll_hcchar_chan_is_enabled(chan_regs)) {
        usb_dwc_ll_hcchar_disable_chan(chan_regs);
        for (int i = 0; i < 5; i++) {
            vTaskDelay(pdMS_TO_TICKS(1));
            if (!usb_dwc_ll_hcchar_chan_is_enabled(chan_regs)) break;
        }
    }
    
    // Clear any previous interrupts before configuring
    usb_dwc_ll_hcint_read_and_clear_intrs(chan_regs);
    
    // Configure channel for IN transaction (read from device)
    // Channel settings (ep_num, dev_addr, ep_type, mps) persist from SETUP stage
    usb_dwc_ll_hcchar_set_dir(chan_regs, 1);  // IN direction
    usb_dwc_ll_hctsiz_init(chan_regs);
    chan_regs->hctsiz_reg.xfersize = max_len;
    chan_regs->hctsiz_reg.pktcnt = (max_len + 63) / 64;
    usb_dwc_ll_hctsiz_set_pid(chan_regs, 1);  // DATA1
    
    // Set DMA buffer for reception - simple direct buffer
    static uint8_t rx_dma_buffer[256] __attribute__((aligned(4)));
    chan_regs->hcdma_reg.dmaaddr = (uint32_t)rx_dma_buffer;
    
    // Clear interrupts AGAIN right before enabling (catches any race conditions)
    usb_dwc_ll_hcint_read_and_clear_intrs(chan_regs);
    
    // Enable channel to start IN transaction
    usb_dwc_ll_hcchar_enable_chan(chan_regs);
    
    // CRITICAL: Small delay to let the transaction START before polling
    // This prevents reading stale CHHLTD from SETUP phase
    vTaskDelay(pdMS_TO_TICKS(1));
    
    // Now poll for completion
    bool first_poll = true;
    while ((xTaskGetTickCount() - start_time) < timeout_ticks) {
        uint32_t hcint = usb_dwc_ll_hcint_read_and_clear_intrs(chan_regs);
        
        // Skip CHHLTD on very first poll (might be stale)
        if (first_poll && (hcint & USB_DWC_LL_INTR_CHAN_CHHLTD) && !(hcint & USB_DWC_LL_INTR_CHAN_XFERCOMPL)) {
            first_poll = false;
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        first_poll = false;
        
        if (hcint & USB_DWC_LL_INTR_CHAN_CHHLTD) {
            if (hcint & USB_DWC_LL_INTR_CHAN_XFERCOMPL) {
                // Data received! Read from DMA buffer
                uint32_t remaining = chan_regs->hctsiz_reg.xfersize;
                *bytes_read = max_len - remaining;
                
                if (*bytes_read > 0 && *bytes_read <= max_len) {
                    memcpy(buffer, rx_dma_buffer, *bytes_read);
                    ESP_LOGI(TAG, "Received %d bytes", *bytes_read);
                    consecutive_failures = 0;  // Reset failure counter on success
                    recovery_attempts = 0;     // Reset recovery attempts on success
                    return ESP_OK;
                }
            }
            // CHHLTD without XFERCOMPL means error - but don't break immediately
            // The device might NAK and we need to retry
            if (hcint & USB_DWC_LL_INTR_CHAN_NAK) {
                // NAK received - device is busy, retry
                usb_dwc_ll_hcchar_enable_chan(chan_regs);  // Re-enable for retry
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }
            break;  // Real error (STALL, error, etc.)
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    
    *bytes_read = 0;
    consecutive_failures++;
    ESP_LOGW(TAG, "Timeout waiting for response (failure %d/%d)", 
             consecutive_failures, MAX_CONSECUTIVE_FAILURES);
    
    // Auto-recovery: Full controller reset after too many consecutive failures
    if (consecutive_failures >= MAX_CONSECUTIVE_FAILURES) {
        recovery_attempts++;
        
        if (recovery_attempts >= MAX_RECOVERY_ATTEMPTS) {
            // Multiple resets failed - device is likely hung, try VBUS power cycle
            ESP_LOGW(TAG, "Power cycling USB (VBUS toggle) - device appears hung");
            
            // Disable VBUS power
            usb_dwc_ll_hprt_dis_pwr(hal_context.dev);
            vTaskDelay(pdMS_TO_TICKS(500));  // Let capacitors discharge
            
            // Re-enable VBUS power
            usb_dwc_ll_hprt_en_pwr(hal_context.dev);
            vTaskDelay(pdMS_TO_TICKS(500));  // Wait for device to power up
            
            // Now do a normal reset
            usb_send_reset_impl();
            recovery_attempts = 0;
            vTaskDelay(pdMS_TO_TICKS(200));
        } else {
            ESP_LOGW(TAG, "Auto-recovery: Full reset after %d consecutive failures (attempt %d/%d)", 
                     consecutive_failures, recovery_attempts, MAX_RECOVERY_ATTEMPTS);
            
            // 1. Flush all FIFOs to clear stale data
            usb_flush_all_fifos();
            
            // 2. Clear all global interrupts
            usb_dwc_ll_gintsts_clear_intrs(hal_context.dev, 0xFFFFFFFF);
            
            // 3. Reset the USB bus
            usb_send_reset_impl();
            
            vTaskDelay(pdMS_TO_TICKS(150));  // Longer recovery time
        }
        
        consecutive_failures = 0;
    }
    
    return ESP_ERR_TIMEOUT;
}

// ============================================================================
// Helper Functions - Create Pre-configured Packets
// ============================================================================

usb_packet_config_t usb_packet_config_default(void) {
    usb_packet_config_t config = {
        .bmRequestType = 0x80,  // Device to Host, Standard, Device
        .bRequest = 0x06,       // GET_DESCRIPTOR
        .wValue = 0x0100,       // Device descriptor
        .wIndex = 0x0000,
        .wLength = USB_DEVICE_DESCRIPTOR_SIZE,
        .packet_size = USB_SETUP_PACKET_SIZE,
        .device_addr = 0,
        .endpoint = 0,
        .max_packet_size = USB_CONTROL_EP0_MPS,
        .expect_response = true,
        .timeout_ms = USB_DEFAULT_TIMEOUT_MS,
        .max_nak_retries = -1,  // Use default
        .response_buffer = NULL,
        .response_buffer_size = 0,
        .bytes_received = NULL
    };
    memset(config.extra_data, 0, sizeof(config.extra_data));
    return config;
}

static esp_err_t usb_get_device_info_impl(usb_device_info_t *info) {
    if (!info) {
        return ESP_ERR_INVALID_ARG;
    }
    
    memset(info, 0, sizeof(usb_device_info_t));
    
    // Check if device is connected
    if (!usb_is_device_connected_impl()) {
        info->connected = false;
        cached_device_info.connected = false;  // Clear cache when disconnected
        return ESP_FAIL;
    }
    
    info->connected = true;
    
    // Return cached info if available (don't spam descriptor requests)
    if (cached_device_info.connected) {
        memcpy(info, &cached_device_info, sizeof(usb_device_info_t));
        return ESP_OK;
    }
    
    // Send reset if device recently connected (before fetching descriptor)
    extern bool usb_needs_reset;
    if (usb_needs_reset) {
        ESP_LOGI(TAG, "Sending USB reset before fetching device info...");
        usb_send_reset_impl();
        usb_needs_reset = false;
        
        // IMPORTANT: Wait for device to be ready after reset
        // USB spec requires at least 10ms, but some devices need more
        ESP_LOGI(TAG, "Waiting 100ms for device to be ready after reset...");
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    // Only fetch descriptor if not cached
    ESP_LOGI(TAG, "Fetching device descriptor for device info...");
    uint8_t desc_buffer[USB_DEVICE_DESCRIPTOR_SIZE];
    size_t bytes_received = 0;
    
    usb_packet_config_t config = {
        .bmRequestType = 0x80,
        .bRequest = 0x06,
        .wValue = 0x0100,  // Device descriptor
        .wIndex = 0x0000,
        .wLength = USB_DEVICE_DESCRIPTOR_SIZE,
        .packet_size = USB_SETUP_PACKET_SIZE,
        .device_addr = 0,
        .endpoint = 0,
        .max_packet_size = USB_CONTROL_EP0_MPS,
        .expect_response = true,
        .timeout_ms = 500,
        .max_nak_retries = 50,
        .response_buffer = desc_buffer,
        .response_buffer_size = sizeof(desc_buffer),
        .bytes_received = &bytes_received
    };
    
    // IMPORTANT: Call _impl directly since we're already on the worker!
    esp_err_t ret = usb_send_packet_impl(&config);
    
    if (ret == ESP_OK && bytes_received >= USB_DEVICE_DESCRIPTOR_SIZE) {
        // Parse device descriptor
        info->vid = desc_buffer[8] | (desc_buffer[9] << 8);
        info->pid = desc_buffer[10] | (desc_buffer[11] << 8);
        info->device_class = desc_buffer[4];
        info->device_subclass = desc_buffer[5];
        info->device_protocol = desc_buffer[6];
        info->max_packet_size = desc_buffer[7];
        
        // Cache the info
        memcpy(&cached_device_info, info, sizeof(usb_device_info_t));
        cached_device_info.connected = true;
        
        // TODO: Get string descriptors if needed
        snprintf(info->manufacturer, sizeof(info->manufacturer), "N/A");
        snprintf(info->product, sizeof(info->product), "N/A");
        snprintf(info->serial, sizeof(info->serial), "N/A");
        
        ESP_LOGI(TAG, "Device info cached: VID=0x%04X PID=0x%04X", info->vid, info->pid);
        
        return ESP_OK;
    }
    
    return ESP_FAIL;
    }
