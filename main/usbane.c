/*
 * USBane - USB Backend Router
 * 
 * Unified interface for USB operations that routes to:
 * - DWC2 hardware USB host (Full-Speed/Low-Speed via USB-OTG port)
 * - GPIO soft-host (Full-Speed via GPIO bit-banging) [WIP - see LIMITATIONS.md]
 *
 * ALL USB operations execute on Core 1 via a dedicated worker task.
 *
 * Copyright 2026 Fehr GmbH
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "usbane.h"
#include "dwc2_backend.h"
#include "soft_backend.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "USBANE";
#define NVS_NAMESPACE "usb_config"

// ============================================================================
// Soft-Host Callback Stubs (required by the library)
// ============================================================================

void led(int on_off)
{
    (void)on_off;  // Not used
}

void usbMess(uint8_t src, uint8_t len, uint8_t *data)
{
    ESP_LOGD(TAG, "Soft-host msg: port=%d len=%d", src, len);
    (void)data;  // Data accessed via usb_get_response_buffer()
}

// ============================================================================
// Backend State
// ============================================================================

static usb_backend_type_t s_current_backend = USB_BACKEND_DWC2;
static bool s_initialized = false;

static usb_soft_host_pins_t s_soft_host_pins = {
    .dp_pin = 16,
    .dm_pin = 17
};

// ============================================================================
// Core 1 Worker Task Infrastructure
// ============================================================================

typedef enum {
    USB_OP_NONE = 0,
    USB_OP_INIT,
    USB_OP_RESET,
    USB_OP_SEND_PACKET,
    USB_OP_ENDPOINT_IN,
    USB_OP_ENDPOINT_OUT,
} usb_op_type_t;

typedef struct {
    usb_op_type_t type;
    
    // For USB_OP_SEND_PACKET
    usb_packet_config_t packet_config;
    
    // For USB_OP_ENDPOINT_IN / OUT
    struct {
        uint8_t endpoint;
        uint8_t device_addr;
        usb_endpoint_type_t ep_type;
        uint8_t data_toggle;    // Used as channel number
        int8_t data_pid;        // Data PID: 0=DATA0, 2=DATA1, -1=auto
        uint8_t *data;
        size_t len;
        uint32_t timeout_ms;
        size_t *bytes_transferred;
    } ep;
    
    // Result
    esp_err_t result;
    volatile bool complete;
} pending_usb_op_t;

static pending_usb_op_t s_pending_op = {0};
static SemaphoreHandle_t s_job_ready_sem = NULL;
static SemaphoreHandle_t s_job_done_sem = NULL;
static TaskHandle_t s_worker_task = NULL;

// Maximum time to wait for soft-host transfer
#define SOFT_HOST_TIMEOUT_MS 1000

// ============================================================================
// Soft-Host Transfer Implementations (run on Core 1)
// ============================================================================

static esp_err_t soft_host_control_transfer_impl(const usb_packet_config_t *config)
{
    usb_set_current_port(0);
    
    // Check if device is enumerated (fsm_state >= 100 means ready for manual requests)
    int cb_cmd, fsm, ack, nack, wire;
    usb_get_debug_info(0, &cb_cmd, &fsm, &ack, &nack, &wire);
    
    if (fsm < 100) {
        ESP_LOGW(TAG, "Soft-host: Device not enumerated yet (fsm=%d cb=%d wire=0x%02x), waiting...", 
                 fsm, cb_cmd, wire);
        // Give FSM time to enumerate
        int last_fsm = -1;
        for (int i = 0; i < 500 && fsm < 100; i++) {
            usb_process();
            vTaskDelay(pdMS_TO_TICKS(10));
            usb_get_debug_info(0, &cb_cmd, &fsm, &ack, &nack, &wire);
            if (fsm != last_fsm) {
                ESP_LOGI(TAG, "Soft-host: FSM progress: fsm=%d cb=%d ack=%d nack=%d wire=0x%02x", 
                         fsm, cb_cmd, ack, nack, wire);
                last_fsm = fsm;
            }
            // If wire goes to 0, device disconnected
            if (wire == 0 && i > 10) {
                ESP_LOGW(TAG, "Soft-host: Device disconnected (wire=0x00)");
                break;
            }
        }
        if (fsm < 100) {
            ESP_LOGE(TAG, "Soft-host: Device enumeration failed (fsm=%d wire=0x%02x)", fsm, wire);
            return ESP_ERR_TIMEOUT;
        }
        ESP_LOGI(TAG, "Soft-host: Device enumerated (fsm=%d)", fsm);
    }
    
    bool is_in = (config->bmRequestType & 0x80) != 0;
    bool is_oversized = config->packet_size > 8;
    
    if (is_oversized) {
        // Oversized SETUP packet (exploit mode) - append extra bytes to SETUP
        ESP_LOGI(TAG, "Soft-host: Oversized SETUP (%d bytes, +%d extra)",
                 (int)config->packet_size, (int)config->extra_data_len);
        RequestOversized(T_SETUP, config->device_addr, 0, T_DATA0,
                         config->bmRequestType, config->bRequest,
                         config->wValue, config->wIndex, config->wLength,
                         is_in ? config->wLength : 0,
                         config->extra_data, config->extra_data_len);
    } else if (is_in || config->extra_data_len == 0) {
        // Standard Control IN or no-data: SETUP -> IN data stage
        Request(T_SETUP, config->device_addr, 0, T_DATA0,
                config->bmRequestType, config->bRequest,
                config->wValue, config->wIndex, config->wLength, config->wLength);
    } else {
        // Standard Control OUT: SETUP -> OUT data stage
        RequestSend(T_SETUP, config->device_addr, 0, T_DATA0,
                    config->bmRequestType, config->bRequest,
                    config->wValue, config->wIndex, config->wLength,
                    config->extra_data_len, (uint8_t*)config->extra_data);
    }
    
    // Poll for completion
    uint32_t start = xTaskGetTickCount();
    uint32_t timeout_ticks = pdMS_TO_TICKS(config->timeout_ms ? config->timeout_ms : SOFT_HOST_TIMEOUT_MS);
    
    int poll_count = 0;
    while (!usb_is_transfer_complete(0)) {
        usb_process();
        poll_count++;
        
        if ((xTaskGetTickCount() - start) > timeout_ticks) {
            // Get debug info from soft-host
            int cb_cmd, fsm, ack, nack, wire;
            usb_get_debug_info(0, &cb_cmd, &fsm, &ack, &nack, &wire);
            ESP_LOGW(TAG, "Soft-host timeout after %d polls: cb_Cmd=%d fsm=%d ack=%d nack=%d wire=0x%02x",
                     poll_count, cb_cmd, fsm, ack, nack, wire);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
    
    // Copy response
    if (is_in && config->response_buffer != NULL) {
        uint8_t resp_len = 0;
        uint8_t *resp = usb_get_response_buffer(0, &resp_len);
        
        if (resp && resp_len > 0) {
            size_t copy_len = (resp_len < config->response_buffer_size) ? resp_len : config->response_buffer_size;
            memcpy(config->response_buffer, resp, copy_len);
            if (config->bytes_received) *config->bytes_received = copy_len;
        } else {
            if (config->bytes_received) *config->bytes_received = 0;
        }
    } else {
        if (config->bytes_received) *config->bytes_received = 0;
    }
    
    return ESP_OK;
}

static esp_err_t soft_host_endpoint_in_impl(
    uint8_t endpoint, uint8_t addr, uint8_t *data, size_t max_len,
    uint32_t timeout_ms, size_t *bytes_received)
{
    usb_set_current_port(0);
    
    RequestIn(T_IN, addr, endpoint, max_len);
    
    uint32_t start = xTaskGetTickCount();
    uint32_t timeout_ticks = pdMS_TO_TICKS(timeout_ms ? timeout_ms : SOFT_HOST_TIMEOUT_MS);
    
    while (!usb_is_transfer_complete(0)) {
        usb_process();
        
        if ((xTaskGetTickCount() - start) > timeout_ticks) {
            ESP_LOGW(TAG, "Soft-host EP IN timeout");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
    
    // Copy response
    uint8_t resp_len = 0;
    uint8_t *resp = usb_get_response_buffer(0, &resp_len);
    
    if (resp && resp_len > 0 && data != NULL) {
        size_t copy_len = (resp_len < max_len) ? resp_len : max_len;
        memcpy(data, resp, copy_len);
        if (bytes_received) *bytes_received = copy_len;
    } else {
        if (bytes_received) *bytes_received = 0;
    }
    
    return ESP_OK;
}

// ============================================================================
// Core 1 Worker Task
// ============================================================================

static void usb_worker_task(void *arg)
{
    ESP_LOGI(TAG, "USB worker started on Core %d", xPortGetCoreID());
    
    while (1) {
        // For soft-host: poll every 1ms to maintain USB SOF timing (USB spec: 1ms ± 0.05ms)
        // For DWC2: wait indefinitely for jobs
        TickType_t wait_time = (s_current_backend == USB_BACKEND_SOFT_HOST) ? pdMS_TO_TICKS(1) : portMAX_DELAY;
        
        BaseType_t got_job = xSemaphoreTake(s_job_ready_sem, wait_time);
        
        // For soft-host: always run usb_process() to advance FSM (device detection/enumeration)
        if (s_current_backend == USB_BACKEND_SOFT_HOST && s_initialized) {
            usb_process();
        }
        
        // If no job, continue polling
        if (got_job != pdTRUE) {
            continue;
        }
        
        esp_err_t result = ESP_OK;
        
        switch (s_pending_op.type) {
            case USB_OP_INIT:
                ESP_LOGI(TAG, "Worker: Initializing backend %s", 
                         usb_backend_type_name(s_current_backend));
                
                if (s_current_backend == USB_BACKEND_DWC2) {
                    // Initialize DWC2 directly (we're already on Core 1)
                    result = dwc2_init_impl();
                } else {
                    // Initialize soft-host GPIO
                    ESP_LOGI(TAG, "Soft-host init: D+=%d D-=%d",
                             s_soft_host_pins.dp_pin, s_soft_host_pins.dm_pin);
                    initStates(
                        s_soft_host_pins.dp_pin, s_soft_host_pins.dm_pin,
                        -1, -1, -1, -1, -1, -1
                    );
                    result = ESP_OK;
                }
                s_initialized = (result == ESP_OK);
                break;
                
            case USB_OP_RESET:
                if (s_current_backend == USB_BACKEND_DWC2) {
                    result = dwc2_send_reset_impl();
                } else {
                    // Soft-host reset: reinit GPIO
                    initStates(
                        s_soft_host_pins.dp_pin, s_soft_host_pins.dm_pin,
                        -1, -1, -1, -1, -1, -1
                    );
                    result = ESP_OK;
                }
                break;
                
            case USB_OP_SEND_PACKET:
                if (s_current_backend == USB_BACKEND_DWC2) {
                    result = dwc2_send_packet_impl(&s_pending_op.packet_config);
                } else {
                    result = soft_host_control_transfer_impl(&s_pending_op.packet_config);
                }
                break;
                
            case USB_OP_ENDPOINT_IN:
                if (s_current_backend == USB_BACKEND_DWC2) {
                    usb_endpoint_params_t params = {
                        .endpoint = s_pending_op.ep.endpoint,
                        .device_addr = s_pending_op.ep.device_addr,
                        .ep_type = s_pending_op.ep.ep_type,
                        .channel = s_pending_op.ep.data_toggle,
                        .buffer = s_pending_op.ep.data,
                        .max_len = s_pending_op.ep.len,
                        .timeout_ms = s_pending_op.ep.timeout_ms,
                        .bytes_read = s_pending_op.ep.bytes_transferred,
                        .data_pid = s_pending_op.ep.data_pid
                    };
                    result = dwc2_endpoint_in_impl(&params);
                } else {
                    result = soft_host_endpoint_in_impl(
                        s_pending_op.ep.endpoint,
                        s_pending_op.ep.device_addr,
                        s_pending_op.ep.data,
                        s_pending_op.ep.len,
                        s_pending_op.ep.timeout_ms,
                        s_pending_op.ep.bytes_transferred
                    );
                }
                break;
                
            case USB_OP_ENDPOINT_OUT:
                if (s_current_backend == USB_BACKEND_DWC2) {
                    usb_endpoint_params_t params = {
                        .endpoint = s_pending_op.ep.endpoint,
                        .device_addr = s_pending_op.ep.device_addr,
                        .ep_type = s_pending_op.ep.ep_type,
                        .channel = s_pending_op.ep.data_toggle,
                        .buffer = (uint8_t *)s_pending_op.ep.data,  // Cast away const for struct
                        .max_len = s_pending_op.ep.len,
                        .timeout_ms = s_pending_op.ep.timeout_ms,
                        .bytes_read = NULL
                    };
                    result = dwc2_endpoint_out_impl(&params);
                } else {
                    // Soft-host OUT not implemented yet
                    result = ESP_ERR_NOT_SUPPORTED;
                }
                break;
                
            default:
                result = ESP_ERR_INVALID_ARG;
                break;
        }
        
        s_pending_op.result = result;
        s_pending_op.complete = true;
        xSemaphoreGive(s_job_done_sem);
    }
}

// Helper: post job and wait for completion
static esp_err_t post_job_and_wait(usb_op_type_t op_type, uint32_t timeout_ms)
{
    s_pending_op.type = op_type;
    s_pending_op.complete = false;
    s_pending_op.result = ESP_ERR_TIMEOUT;
    
    xSemaphoreGive(s_job_ready_sem);
    
    TickType_t wait_ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    
    if (xSemaphoreTake(s_job_done_sem, wait_ticks) == pdTRUE) {
        return s_pending_op.result;
    }
    
    return ESP_ERR_TIMEOUT;
}

// ============================================================================
// Public API: Initialization
// ============================================================================

esp_err_t usb_backend_start_worker(void)
{
    if (s_worker_task != NULL) {
        ESP_LOGW(TAG, "Worker already running");
        return ESP_OK;
    }
    
    s_job_ready_sem = xSemaphoreCreateBinary();
    s_job_done_sem = xSemaphoreCreateBinary();
    
    if (!s_job_ready_sem || !s_job_done_sem) {
        ESP_LOGE(TAG, "Failed to create semaphores");
        return ESP_ERR_NO_MEM;
    }
    
    // Create worker task pinned to Core 1
    BaseType_t ret = xTaskCreatePinnedToCore(
        usb_worker_task,
        "usb_worker",
        8192,
        NULL,
        5,  // High priority for USB timing
        &s_worker_task,
        1   // Core 1
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create worker task");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "USB worker task started on Core 1");
    return ESP_OK;
}

esp_err_t usb_backend_init(void)
{
    if (s_worker_task == NULL) {
        ESP_LOGE(TAG, "Worker not started, call usb_backend_start_worker() first");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Load backend config from NVS
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t backend_val = 0;
        if (nvs_get_u8(nvs, "usb_backend", &backend_val) == ESP_OK) {
            s_current_backend = (backend_val == 1) ? USB_BACKEND_SOFT_HOST : USB_BACKEND_DWC2;
        }
        
        if (s_current_backend == USB_BACKEND_SOFT_HOST) {
            int8_t dp = 16, dm = 17;
            nvs_get_i8(nvs, "soft_dp", &dp);
            nvs_get_i8(nvs, "soft_dm", &dm);
            s_soft_host_pins.dp_pin = dp;
            s_soft_host_pins.dm_pin = dm;
        }
        
        nvs_close(nvs);
    }
    
    ESP_LOGI(TAG, "Backend from NVS: %s", usb_backend_type_name(s_current_backend));
    
    // Post init job to Core 1 worker
    return post_job_and_wait(USB_OP_INIT, 5000);
}

esp_err_t usb_backend_deinit(void)
{
    s_initialized = false;
    // Could add cleanup logic here
    return ESP_OK;
}

// ============================================================================
// Public API: Info
// ============================================================================

usb_backend_type_t usb_backend_get_type(void)
{
    return s_current_backend;
}

bool usb_backend_is_soft_host(void)
{
    return s_current_backend == USB_BACKEND_SOFT_HOST;
}

const char *usb_backend_type_name(usb_backend_type_t type)
{
    switch (type) {
        case USB_BACKEND_DWC2:      return "DWC2 Hardware";
        case USB_BACKEND_SOFT_HOST: return "GPIO Soft-Host (LS)";
        default:                    return "Unknown";
    }
}

bool usb_backend_soft_host_available(void)
{
    return true;  // Always available on ESP32-S3
}

// ============================================================================
// Public API: Configuration (before init)
// ============================================================================

esp_err_t usb_backend_set_type(usb_backend_type_t type)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Cannot change backend after init");
        return ESP_ERR_INVALID_STATE;
    }
    s_current_backend = type;
    return ESP_OK;
}

esp_err_t usb_backend_set_soft_host_pins(const usb_soft_host_pins_t *pins)
{
    if (!pins) return ESP_ERR_INVALID_ARG;
    if (s_initialized) return ESP_ERR_INVALID_STATE;
    
    s_soft_host_pins = *pins;
    ESP_LOGI(TAG, "Soft-host pins: D+=%d D-=%d", pins->dp_pin, pins->dm_pin);
    return ESP_OK;
}

esp_err_t usb_backend_get_soft_host_pins(usb_soft_host_pins_t *pins)
{
    if (!pins) return ESP_ERR_INVALID_ARG;
    *pins = s_soft_host_pins;
    return ESP_OK;
}

// ============================================================================
// Public API: USB Operations (all run on Core 1)
// ============================================================================

esp_err_t usb_backend_reset(void)
{
    return post_job_and_wait(USB_OP_RESET, 3000);
}

esp_err_t usb_backend_send_packet(const usb_packet_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    
    // Copy config to pending op
    memcpy(&s_pending_op.packet_config, config, sizeof(usb_packet_config_t));
    
    return post_job_and_wait(USB_OP_SEND_PACKET, config->timeout_ms + 1000);
}

esp_err_t usb_backend_endpoint_in(
    uint8_t endpoint,
    uint8_t device_addr,
    usb_endpoint_type_t ep_type,
    uint8_t data_toggle,
    uint8_t *data,
    size_t max_len,
    uint32_t timeout_ms,
    size_t *bytes_received)
{
    s_pending_op.ep.endpoint = endpoint;
    s_pending_op.ep.device_addr = device_addr;
    s_pending_op.ep.ep_type = ep_type;
    s_pending_op.ep.data_toggle = data_toggle;
    s_pending_op.ep.data_pid = -1;  // Default: auto (DATA0)
    s_pending_op.ep.data = data;
    s_pending_op.ep.len = max_len;
    s_pending_op.ep.timeout_ms = timeout_ms;
    s_pending_op.ep.bytes_transferred = bytes_received;
    
    return post_job_and_wait(USB_OP_ENDPOINT_IN, timeout_ms + 1000);
}

esp_err_t usb_backend_endpoint_in_with_pid(
    uint8_t endpoint,
    uint8_t device_addr,
    usb_endpoint_type_t ep_type,
    uint8_t channel,
    int8_t data_pid,
    uint8_t *data,
    size_t max_len,
    uint32_t timeout_ms,
    size_t *bytes_received)
{
    s_pending_op.ep.endpoint = endpoint;
    s_pending_op.ep.device_addr = device_addr;
    s_pending_op.ep.ep_type = ep_type;
    s_pending_op.ep.data_toggle = channel;
    s_pending_op.ep.data_pid = data_pid;
    s_pending_op.ep.data = data;
    s_pending_op.ep.len = max_len;
    s_pending_op.ep.timeout_ms = timeout_ms;
    s_pending_op.ep.bytes_transferred = bytes_received;
    
    return post_job_and_wait(USB_OP_ENDPOINT_IN, timeout_ms + 1000);
}

esp_err_t usb_backend_endpoint_out(
    uint8_t endpoint,
    uint8_t device_addr,
    usb_endpoint_type_t ep_type,
    uint8_t data_toggle,
    const uint8_t *data,
    size_t len,
    uint32_t timeout_ms)
{
    s_pending_op.ep.endpoint = endpoint;
    s_pending_op.ep.device_addr = device_addr;
    s_pending_op.ep.ep_type = ep_type;
    s_pending_op.ep.data_toggle = data_toggle;
    s_pending_op.ep.data = (uint8_t*)data;  // Cast away const, worker won't modify
    s_pending_op.ep.len = len;
    s_pending_op.ep.timeout_ms = timeout_ms;
    s_pending_op.ep.bytes_transferred = NULL;
    
    return post_job_and_wait(USB_OP_ENDPOINT_OUT, timeout_ms + 1000);
}

bool usb_backend_is_device_connected(void)
{
    if (!s_initialized) return false;
    
    if (s_current_backend == USB_BACKEND_SOFT_HOST) {
        // Check soft-host port 0 for device connection
        return soft_host_is_device_connected(0) != 0;
    } else {
        // DWC2 backend - direct call (no Core 1 wrapper needed for reads)
        return dwc2_is_device_connected();
    }
}
