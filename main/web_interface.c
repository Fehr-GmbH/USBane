/*
 * USBane - Web Interface
 * Allows real-time parameter adjustment via web browser
 */

#include "web_interface.h"
#include "usbane.h"
#include "chain_engine.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include <string.h>
#include <inttypes.h>

// Web Interface Constants

static const char *TAG = "WEB_UI";
static httpd_handle_t server = NULL;

// Note: Auto-reset is handled internally by usbane.c when ctrl pipe isn't allocated

// Webhook trigger storage for chain waitfor actions
#define MAX_TRIGGERS 16
#define TRIGGER_ID_MAX_LEN 32
static char triggered_ids[MAX_TRIGGERS][TRIGGER_ID_MAX_LEN] = {0};
static int triggered_count = 0;
static portMUX_TYPE trigger_mutex = portMUX_INITIALIZER_UNLOCKED;

// HTTP API helpers for synchronous execution
static chain_result_t http_api_results[CHAIN_MAX_ENTRIES];
static int http_api_result_count = 0;
static esp_err_t http_api_final_result = ESP_OK;
static SemaphoreHandle_t http_api_done_sem = NULL;

// Forward declaration for stats update
static void update_usb_stats(size_t bytes_rx, size_t bytes_tx);

static void http_api_result_cb(int index, const chain_result_t *result, void *user_data) {
    // Update USB stats for all USB transfer types
    if (result->type <= CHAIN_TYPE_INTERRUPT_OUT) {
        update_usb_stats(result->bytes_received, 8);
    }
    
    if (index < CHAIN_MAX_ENTRIES) {
        memcpy(&http_api_results[index], result, sizeof(chain_result_t));
        if (index >= http_api_result_count) {
            http_api_result_count = index + 1;
        }
    }
}

static void http_api_done_cb(esp_err_t result, void *user_data) {
    http_api_final_result = result;
    if (http_api_done_sem) {
        xSemaphoreGive(http_api_done_sem);
    }
}

// Performance tracking
typedef struct {
    uint32_t total_requests;
    uint32_t total_bytes_rx;
    uint32_t total_bytes_tx;
    uint32_t last_update_time;
    uint32_t requests_last_second;
    uint32_t bytes_rx_last_second;
    uint32_t bytes_tx_last_second;
    uint8_t cpu_core0_load;  // CPU load percentage (0-100)
    uint8_t cpu_core1_load;  // CPU load percentage (0-100)
    uint32_t heap_free;      // Free heap in bytes
    uint32_t heap_total;     // Total heap in bytes
    uint32_t heap_min_free;  // Minimum free heap ever
} usb_stats_t;

static usb_stats_t usb_stats = {0};
static portMUX_TYPE usb_stats_mutex = portMUX_INITIALIZER_UNLOCKED;

// CPU load tracking - Real FreeRTOS runtime stats
static uint32_t last_idle_runtime_core0 = 0;
static uint32_t last_idle_runtime_core1 = 0;
static uint32_t last_total_runtime = 0;
static uint32_t last_cpu_check_time = 0;

// Forward declarations
static void update_cpu_load(void);

// Update performance statistics (thread-safe)
static void update_usb_stats(size_t bytes_rx, size_t bytes_tx) {
    uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    portENTER_CRITICAL(&usb_stats_mutex);
    
    usb_stats.total_requests++;
    usb_stats.total_bytes_rx += bytes_rx;
    usb_stats.total_bytes_tx += bytes_tx;
    
    // Calculate per-second rates (update every second)
    if (current_time - usb_stats.last_update_time >= 1000) {
        usb_stats.requests_last_second = usb_stats.total_requests;
        usb_stats.bytes_rx_last_second = usb_stats.total_bytes_rx;
        usb_stats.bytes_tx_last_second = usb_stats.total_bytes_tx;
        usb_stats.last_update_time = current_time;
        
        // Reset counters for next second
        usb_stats.total_requests = 0;
        usb_stats.total_bytes_rx = 0;
        usb_stats.total_bytes_tx = 0;
    }
    
    portEXIT_CRITICAL(&usb_stats_mutex);
}

// Embedded files (from CMakeLists.txt EMBED_FILES)
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t app_js_start[]     asm("_binary_app_js_start");
extern const uint8_t app_js_end[]       asm("_binary_app_js_end");
extern const uint8_t logo_svg_start[]   asm("_binary_logo_svg_start");
extern const uint8_t logo_svg_end[]     asm("_binary_logo_svg_end");
extern const uint8_t applogo_png_start[] asm("_binary_applogo_png_start");
extern const uint8_t applogo_png_end[]   asm("_binary_applogo_png_end");
extern const uint8_t apptext_svg_start[] asm("_binary_apptext_svg_start");
extern const uint8_t apptext_svg_end[]   asm("_binary_apptext_svg_end");
extern const uint8_t favicon_ico_start[] asm("_binary_favicon_ico_start");
extern const uint8_t favicon_ico_end[]   asm("_binary_favicon_ico_end");
extern const uint8_t api_html_start[]    asm("_binary_api_html_start");
extern const uint8_t api_html_end[]      asm("_binary_api_html_end");
extern const uint8_t openapi_json_start[] asm("_binary_openapi_json_start");
extern const uint8_t openapi_json_end[]   asm("_binary_openapi_json_end");


// Update CPU load statistics using REAL FreeRTOS runtime stats
// Based on: https://github.com/espressif/esp-idf/blob/master/examples/system/freertos/real_time_stats
static void update_cpu_load(void) {
    uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // Only update every 500ms to avoid overhead
    if (current_time - last_cpu_check_time < 500) {
        return;
    }
    
    // Get number of tasks
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    TaskStatus_t *task_array = (TaskStatus_t *)malloc(task_count * sizeof(TaskStatus_t));
    
    if (task_array == NULL) {
        return;  // Out of memory
    }
    
    // Get task statistics
    uint32_t total_runtime;
    task_count = uxTaskGetSystemState(task_array, task_count, &total_runtime);
    
    if (task_count == 0) {
        free(task_array);
        return;
    }
    
    // Find IDLE tasks for each core and sum their runtime
    uint32_t idle_runtime_core0 = 0;
    uint32_t idle_runtime_core1 = 0;
    
    for (UBaseType_t i = 0; i < task_count; i++) {
        const char *task_name = pcTaskGetName(task_array[i].xHandle);
        
        // IDLE0 and IDLE1 are the idle tasks for each core
        if (strcmp(task_name, "IDLE0") == 0 || strcmp(task_name, "IDLE") == 0) {
            idle_runtime_core0 = task_array[i].ulRunTimeCounter;
        } else if (strcmp(task_name, "IDLE1") == 0) {
            idle_runtime_core1 = task_array[i].ulRunTimeCounter;
        }
    }
    
    free(task_array);
    
    // Calculate CPU load if we have previous measurements
    if (last_cpu_check_time > 0 && last_total_runtime > 0) {
        // Calculate deltas
        uint32_t runtime_delta = total_runtime - last_total_runtime;
        uint32_t idle_delta_core0 = idle_runtime_core0 - last_idle_runtime_core0;
        uint32_t idle_delta_core1 = idle_runtime_core1 - last_idle_runtime_core1;
        
        // For dual core: each core gets half the total runtime
        uint32_t runtime_per_core = runtime_delta / 2;
        
        uint8_t load_core0 = 0;
        uint8_t load_core1 = 0;
        
        if (runtime_per_core > 0) {
            // Calculate idle percentage for each core
            uint32_t idle_pct_core0 = (idle_delta_core0 * 100) / runtime_per_core;
            uint32_t idle_pct_core1 = (idle_delta_core1 * 100) / runtime_per_core;
            
            // CPU load = 100% - idle%
            load_core0 = (idle_pct_core0 >= 100) ? 0 : (100 - idle_pct_core0);
            load_core1 = (idle_pct_core1 >= 100) ? 0 : (100 - idle_pct_core1);
        }
        
        // Get heap statistics
        uint32_t heap_free = esp_get_free_heap_size();
        uint32_t heap_min_free = esp_get_minimum_free_heap_size();
        
        // Get total heap size (internal + PSRAM if available)
        multi_heap_info_t heap_info;
        heap_caps_get_info(&heap_info, MALLOC_CAP_DEFAULT);
        uint32_t heap_total = heap_info.total_free_bytes + heap_info.total_allocated_bytes;
        
        // Update stats (thread-safe)
        portENTER_CRITICAL(&usb_stats_mutex);
        usb_stats.cpu_core0_load = load_core0;
        usb_stats.cpu_core1_load = load_core1;
        usb_stats.heap_free = heap_free;
        usb_stats.heap_total = heap_total;
        usb_stats.heap_min_free = heap_min_free;
        portEXIT_CRITICAL(&usb_stats_mutex);
    }
    
    // Save current values for next calculation
    last_idle_runtime_core0 = idle_runtime_core0;
    last_idle_runtime_core1 = idle_runtime_core1;
    last_total_runtime = total_runtime;
    last_cpu_check_time = current_time;
}

// API handler: Single USB request (control, bulk, or interrupt - uses executor on Core 1)
// POST /api/single_request?type=control&bmRequestType=0x80&bRequest=0x06...
// POST /api/single_request?type=bulk_in&ep=1&len=64&addr=0...
// POST /api/single_request?type=bulk_out&ep=1&data=AABBCC...
static esp_err_t api_single_request_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "=== API: single_request handler called ===");
    
    // Check if executor is busy
    if (usb_executor_is_busy()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Executor busy\"}");
        return ESP_OK;
    }
    
    // Parse query parameters - initialize with defaults
    usb_job_single_t params = {0};
    params.type = USB_TRANSFER_CONTROL;  // Default to control transfer
    params.bmRequestType = 0x80;
    params.bRequest = 0x06;
    params.wValue = 0x0100;
    params.wIndex = 0x0000;
    params.wLength = 18;
    params.packetSize = 8;
    params.deviceAddr = 0;
    params.timeout = 1000;
    params.endpoint = 1;
    params.channel = 1;
    params.dataLen = 64;  // Default length for IN transfers
    
    static char query[512];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[64];
        
        // Parse transfer type first
        if (httpd_query_key_value(query, "type", val, sizeof(val)) == ESP_OK) {
            if (strcmp(val, "control") == 0) {
                params.type = USB_TRANSFER_CONTROL;
            } else if (strcmp(val, "bulk_in") == 0) {
                params.type = USB_TRANSFER_BULK_IN;
            } else if (strcmp(val, "bulk_out") == 0) {
                params.type = USB_TRANSFER_BULK_OUT;
            } else if (strcmp(val, "interrupt_in") == 0) {
                params.type = USB_TRANSFER_INTERRUPT_IN;
            } else if (strcmp(val, "interrupt_out") == 0) {
                params.type = USB_TRANSFER_INTERRUPT_OUT;
            }
        }
        
        // Common parameters
        if (httpd_query_key_value(query, "addr", val, sizeof(val)) == ESP_OK)
            params.deviceAddr = (uint8_t)atoi(val);
        if (httpd_query_key_value(query, "deviceAddr", val, sizeof(val)) == ESP_OK)
            params.deviceAddr = (uint8_t)atoi(val);
        if (httpd_query_key_value(query, "timeout", val, sizeof(val)) == ESP_OK) {
            params.timeout = (uint32_t)atoi(val);
            if (params.timeout < 10) params.timeout = 10;
            if (params.timeout > 30000) params.timeout = 30000;
        }
        
        // Control transfer parameters
        if (httpd_query_key_value(query, "bmRequestType", val, sizeof(val)) == ESP_OK)
            params.bmRequestType = (uint8_t)strtol(val, NULL, 0);
        if (httpd_query_key_value(query, "bRequest", val, sizeof(val)) == ESP_OK)
            params.bRequest = (uint8_t)strtol(val, NULL, 0);
        if (httpd_query_key_value(query, "wValue", val, sizeof(val)) == ESP_OK)
            params.wValue = (uint16_t)strtol(val, NULL, 0);
        if (httpd_query_key_value(query, "wIndex", val, sizeof(val)) == ESP_OK)
            params.wIndex = (uint16_t)strtol(val, NULL, 0);
        if (httpd_query_key_value(query, "wLength", val, sizeof(val)) == ESP_OK)
            params.wLength = (uint16_t)atoi(val);
        if (httpd_query_key_value(query, "packetSize", val, sizeof(val)) == ESP_OK) {
            params.packetSize = (uint8_t)atoi(val);
            if (params.packetSize == 0) params.packetSize = 8;
        }
        if (httpd_query_key_value(query, "dataStageEp", val, sizeof(val)) == ESP_OK)
            params.dataStageEp = (uint8_t)atoi(val);
        if (httpd_query_key_value(query, "setupOnly", val, sizeof(val)) == ESP_OK)
            params.setupOnly = (atoi(val) != 0) || (strcmp(val, "true") == 0);
        
        // Endpoint transfer parameters
        if (httpd_query_key_value(query, "ep", val, sizeof(val)) == ESP_OK)
            params.endpoint = (uint8_t)atoi(val);
        if (httpd_query_key_value(query, "channel", val, sizeof(val)) == ESP_OK)
            params.channel = (uint8_t)atoi(val);
        if (httpd_query_key_value(query, "len", val, sizeof(val)) == ESP_OK)
            params.dataLen = (uint16_t)atoi(val);
        if (httpd_query_key_value(query, "continuous", val, sizeof(val)) == ESP_OK)
            params.continuous = (atoi(val) != 0);
        if (httpd_query_key_value(query, "max_attempts", val, sizeof(val)) == ESP_OK)
            params.maxAttempts = (uint32_t)atoi(val);
        
        // Parse data bytes (hex string) - for OUT transfers and control with data
        static char dataBytes_str[512];
        if (httpd_query_key_value(query, "data", dataBytes_str, sizeof(dataBytes_str)) == ESP_OK && strlen(dataBytes_str) > 0) {
            params.dataLen = 0;  // Reset for OUT transfers
            const char *hex = dataBytes_str;
            while (*hex && params.dataLen < sizeof(params.data)) {
                while (*hex == ' ' || *hex == ',') hex++;
                if (!*hex) break;
                char byte_str[3] = {hex[0], hex[1] ? hex[1] : 0, 0};
                params.data[params.dataLen++] = (uint8_t)strtoul(byte_str, NULL, 16);
                hex += byte_str[1] ? 2 : 1;
            }
        }
    }
    
    // Log based on type
    const char *type_str[] = {"control", "bulk_in", "bulk_out", "interrupt_in", "interrupt_out"};
    if (params.type == USB_TRANSFER_CONTROL) {
        ESP_LOGI(TAG, "Single %s: bmRT=0x%02x bReq=0x%02x wVal=0x%04x wIdx=0x%04x wLen=%d",
                 type_str[params.type], params.bmRequestType, params.bRequest, params.wValue, params.wIndex, params.wLength);
    } else {
        ESP_LOGI(TAG, "Single %s: ep=%d addr=%d len=%d timeout=%"PRIu32"",
                 type_str[params.type], params.endpoint, params.deviceAddr, params.dataLen, params.timeout);
    }
    
    // Build CSV line from params (single request = chain with 1 entry)
    char csv[512];
    int csv_len = 0;
    
    switch (params.type) {
        case USB_TRANSFER_CONTROL:
            csv_len = snprintf(csv, sizeof(csv), "control,0x%02x,0x%02x,0x%04x,0x%04x,%d,%d,%d,%d",
                params.bmRequestType, params.bRequest, params.wValue, params.wIndex,
                params.wLength, params.packetSize, params.deviceAddr, params.dataStageEp);
            break;
        case USB_TRANSFER_BULK_IN:
            csv_len = snprintf(csv, sizeof(csv), "bulk_in,%d,%d,%"PRIu32",%d,%d",
                params.endpoint, params.dataLen, params.timeout, params.deviceAddr, params.channel);
            break;
        case USB_TRANSFER_BULK_OUT: {
            csv_len = snprintf(csv, sizeof(csv), "bulk_out,%d,", params.endpoint);
            // Append data bytes as hex
            for (int i = 0; i < params.dataLen && csv_len < (int)sizeof(csv) - 10; i++) {
                csv_len += snprintf(csv + csv_len, sizeof(csv) - csv_len, "%02X", params.data[i]);
            }
            csv_len += snprintf(csv + csv_len, sizeof(csv) - csv_len, ",%"PRIu32",%d,%d",
                params.timeout, params.deviceAddr, params.channel);
            break;
        }
        case USB_TRANSFER_INTERRUPT_IN:
            csv_len = snprintf(csv, sizeof(csv), "interrupt_in,%d,%d,%"PRIu32",%d,%d",
                params.endpoint, params.dataLen, params.timeout, params.deviceAddr, params.channel);
            break;
        case USB_TRANSFER_INTERRUPT_OUT: {
            csv_len = snprintf(csv, sizeof(csv), "interrupt_out,%d,", params.endpoint);
            for (int i = 0; i < params.dataLen && csv_len < (int)sizeof(csv) - 10; i++) {
                csv_len += snprintf(csv + csv_len, sizeof(csv) - csv_len, "%02X", params.data[i]);
            }
            csv_len += snprintf(csv + csv_len, sizeof(csv) - csv_len, ",%"PRIu32",%d,%d",
                params.timeout, params.deviceAddr, params.channel);
            break;
        }
    }
    
    ESP_LOGI(TAG, "API single as chain: %s", csv);
    
    // Initialize semaphore for blocking wait
    if (!http_api_done_sem) {
        http_api_done_sem = xSemaphoreCreateBinary();
    }
    
    // Reset result storage
    http_api_result_count = 0;
    http_api_final_result = ESP_OK;
    xSemaphoreTake(http_api_done_sem, 0);
    
    // Copy CSV to heap (executor will free it)
    char *csv_copy = strdup(csv);
    if (!csv_copy) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Out of memory\"}");
        return ESP_OK;
    }
    
    // Submit as chain - unified path!
    esp_err_t submit_ret = usb_executor_submit_chain(csv_copy, csv_len, http_api_result_cb, NULL, http_api_done_cb, NULL);
    
    if (submit_ret != ESP_OK) {
        free(csv_copy);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Failed to submit job\"}");
        return ESP_OK;
    }
    
    // Wait for completion (max 30 seconds)
    if (xSemaphoreTake(http_api_done_sem, pdMS_TO_TICKS(30000)) != pdTRUE) {
        usb_executor_stop();
        httpd_resp_set_status(req, "504 Gateway Timeout");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Execution timeout\"}");
        return ESP_OK;
    }
    
    // Build response from result
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", (http_api_result_count > 0 && http_api_results[0].status == 0) ? "success" : "failed");
    cJSON_AddNumberToObject(root, "bytes_received", http_api_result_count > 0 ? http_api_results[0].bytes_received : 0);
    
    if (http_api_result_count > 0 && http_api_results[0].data_len > 0) {
        size_t data_len = http_api_results[0].data_len;
        char *hex_str = malloc(data_len * 3 + 1);
        char *ascii_str = malloc(data_len + 1);
        if (hex_str && ascii_str) {
            for (size_t i = 0; i < data_len; i++) {
                sprintf(hex_str + i * 3, "%02x ", http_api_results[0].data[i]);
                char c = http_api_results[0].data[i];
                ascii_str[i] = (c >= 32 && c <= 126) ? c : '.';
            }
            hex_str[data_len * 3] = '\0';
            ascii_str[data_len] = '\0';
            cJSON_AddStringToObject(root, "data", hex_str);
            cJSON_AddStringToObject(root, "ascii", ascii_str);
            free(hex_str);
            free(ascii_str);
        }
    } else {
        cJSON_AddStringToObject(root, "data", "");
        cJSON_AddStringToObject(root, "ascii", "");
    }
    
    const char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    free((void *)json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

// API handler: Connection status
static esp_err_t api_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "connected", usb_is_device_connected());
    
    const char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    
    free((void *)json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

// API handler: Get app version
static esp_err_t api_version_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", CONFIG_USBANE_VERSION);
    
    const char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    
    free((void *)json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

// API handler: USB throughput statistics (thread-safe)
static esp_err_t api_stats_handler(httpd_req_t *req)
{
    // Update CPU load before reading stats
    update_cpu_load();
    
    // Debug: Log raw stats values
    ESP_LOGD(TAG, "Stats: req=%"PRIu32" rx=%"PRIu32" tx=%"PRIu32" cpu0=%d cpu1=%d",
             usb_stats.requests_last_second, usb_stats.bytes_rx_last_second,
             usb_stats.bytes_tx_last_second, usb_stats.cpu_core0_load, usb_stats.cpu_core1_load);
    
    // Read stats atomically
    portENTER_CRITICAL(&usb_stats_mutex);
    uint32_t requests_per_sec = usb_stats.requests_last_second;
    uint32_t bytes_rx_per_sec = usb_stats.bytes_rx_last_second;
    uint32_t bytes_tx_per_sec = usb_stats.bytes_tx_last_second;
    uint8_t cpu_core0_load = usb_stats.cpu_core0_load;
    uint8_t cpu_core1_load = usb_stats.cpu_core1_load;
    uint32_t heap_free = usb_stats.heap_free;
    uint32_t heap_total = usb_stats.heap_total;
    uint32_t heap_min_free = usb_stats.heap_min_free;
    portEXIT_CRITICAL(&usb_stats_mutex);
    
    cJSON *root = cJSON_CreateObject();
    
    cJSON_AddNumberToObject(root, "requests_per_sec", requests_per_sec);
    cJSON_AddNumberToObject(root, "bytes_rx_per_sec", bytes_rx_per_sec);
    cJSON_AddNumberToObject(root, "bytes_tx_per_sec", bytes_tx_per_sec);
    cJSON_AddNumberToObject(root, "cpu_core0_load", cpu_core0_load);
    cJSON_AddNumberToObject(root, "cpu_core1_load", cpu_core1_load);
    cJSON_AddNumberToObject(root, "heap_free", heap_free);
    cJSON_AddNumberToObject(root, "heap_total", heap_total);
    cJSON_AddNumberToObject(root, "heap_min_free", heap_min_free);
    
    const char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    
    free((void *)json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

// API handler: Get WiFi configuration
static esp_err_t api_wifi_config_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    
    nvs_handle_t nvs;
    if (nvs_open("wifi_config", NVS_READONLY, &nvs) == ESP_OK) {
        char mode[8] = "ap";
        char sta_ssid[33] = {0};
        char ap_ssid[33] = "USBane";
        size_t len;
        
        len = sizeof(mode);
        nvs_get_str(nvs, "mode", mode, &len);
        
        len = sizeof(sta_ssid);
        nvs_get_str(nvs, "sta_ssid", sta_ssid, &len);
        
        len = sizeof(ap_ssid);
        nvs_get_str(nvs, "ap_ssid", ap_ssid, &len);
        
        nvs_close(nvs);
        
        cJSON_AddStringToObject(root, "mode", mode);
        cJSON_AddStringToObject(root, "sta_ssid", sta_ssid);
        cJSON_AddStringToObject(root, "ap_ssid", ap_ssid);
    } else {
        cJSON_AddStringToObject(root, "mode", "ap");
        cJSON_AddStringToObject(root, "ap_ssid", "USBane");
        cJSON_AddStringToObject(root, "sta_ssid", "");
    }
    
    // Check if connected in STA mode
    wifi_mode_t wifi_mode;
    esp_wifi_get_mode(&wifi_mode);
    
    if (wifi_mode == WIFI_MODE_STA || wifi_mode == WIFI_MODE_APSTA) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            cJSON_AddBoolToObject(root, "connected", true);
            cJSON_AddNumberToObject(root, "rssi", ap_info.rssi);
            
            // Get IP address
            esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (sta_netif) {
                esp_netif_ip_info_t ip_info;
                if (esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
                    char ip_str[16];
                    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
                    cJSON_AddStringToObject(root, "ip", ip_str);
                }
            }
        } else {
            cJSON_AddBoolToObject(root, "connected", false);
        }
    } else {
        cJSON_AddBoolToObject(root, "connected", false);
    }
    
    const char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    
    free((void *)json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}


// API handler: Device info
static esp_err_t api_device_info_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "=== API: device_info handler called ===");
    
    usb_device_info_t info;
    esp_err_t ret = usb_get_device_info(&info);
    
    cJSON *root = cJSON_CreateObject();
    
    if (ret == ESP_OK && info.connected) {
        cJSON_AddBoolToObject(root, "connected", true);
        
        // Format VID/PID as hex strings
        char vid_str[16], pid_str[16];
        snprintf(vid_str, sizeof(vid_str), "0x%04X", info.vid);
        snprintf(pid_str, sizeof(pid_str), "0x%04X", info.pid);
        
        cJSON_AddStringToObject(root, "vid", vid_str);
        cJSON_AddStringToObject(root, "pid", pid_str);
        cJSON_AddNumberToObject(root, "device_class", info.device_class);
        cJSON_AddNumberToObject(root, "device_subclass", info.device_subclass);
        cJSON_AddNumberToObject(root, "device_protocol", info.device_protocol);
        cJSON_AddNumberToObject(root, "max_packet_size", info.max_packet_size);
        cJSON_AddStringToObject(root, "manufacturer", info.manufacturer);
        cJSON_AddStringToObject(root, "product", info.product);
        cJSON_AddStringToObject(root, "serial", info.serial);
    } else {
        cJSON_AddBoolToObject(root, "connected", false);
    }
    
    const char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    
    free((void *)json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

// API handler: Manual USB Reset
static esp_err_t api_reset_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "API: Manual USB Reset requested");
    
    if (!usb_is_device_connected()) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "status", "error");
        cJSON_AddStringToObject(root, "message", "No device connected");
        
        const char *json_str = cJSON_Print(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json_str);
        
        free((void *)json_str);
        cJSON_Delete(root);
        return ESP_OK;
    }
    
    // Send USB reset
    esp_err_t ret = usb_send_reset();
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", ret == ESP_OK ? "success" : "failed");
    cJSON_AddBoolToObject(root, "connected", usb_is_device_connected());
    
    const char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    
    free((void *)json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

// API handler: Save configuration and reboot
static esp_err_t api_save_config_handler(httpd_req_t *req)
{
    static char query[512];
    
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char otg_mode_str[8] = {0};
        char otg_speed_str[8] = {0};
        char wifi_mode_str[8] = {0};
        char sta_ssid[33] = {0};
        char sta_password[65] = {0};
        char ap_ssid[33] = {0};
        char ap_password[65] = {0};
        
        uint8_t otg_mode = 0;  // Default: Host
        uint8_t otg_speed = 1; // Default: Full-Speed
        
        if (httpd_query_key_value(query, "otgMode", otg_mode_str, sizeof(otg_mode_str)) == ESP_OK) {
            otg_mode = (uint8_t)atoi(otg_mode_str);
        }
        
        if (httpd_query_key_value(query, "otgSpeed", otg_speed_str, sizeof(otg_speed_str)) == ESP_OK) {
            otg_speed = (uint8_t)atoi(otg_speed_str);
        }
        
        // Parse WiFi settings
        httpd_query_key_value(query, "wifiMode", wifi_mode_str, sizeof(wifi_mode_str));
        httpd_query_key_value(query, "staSsid", sta_ssid, sizeof(sta_ssid));
        httpd_query_key_value(query, "staPassword", sta_password, sizeof(sta_password));
        httpd_query_key_value(query, "apSsid", ap_ssid, sizeof(ap_ssid));
        httpd_query_key_value(query, "apPassword", ap_password, sizeof(ap_password));
        
        ESP_LOGI(TAG, "API: Save config - otg_mode=%d, otg_speed=%d, wifi_mode=%s", otg_mode, otg_speed, wifi_mode_str);
        
        // Save USB PHY config to NVS
        esp_err_t ret = usb_save_phy_config(otg_mode, otg_speed);
        
        // Save WiFi config to NVS
        if (strlen(wifi_mode_str) > 0) {
            nvs_handle_t nvs;
            if (nvs_open("wifi_config", NVS_READWRITE, &nvs) == ESP_OK) {
                nvs_set_str(nvs, "mode", wifi_mode_str);
                if (strlen(sta_ssid) > 0) nvs_set_str(nvs, "sta_ssid", sta_ssid);
                if (strlen(sta_password) > 0) nvs_set_str(nvs, "sta_pass", sta_password);
                if (strlen(ap_ssid) > 0) nvs_set_str(nvs, "ap_ssid", ap_ssid);
                if (strlen(ap_password) > 0) nvs_set_str(nvs, "ap_pass", ap_password);
                nvs_commit(nvs);
                nvs_close(nvs);
                ESP_LOGI(TAG, "WiFi config saved: mode=%s", wifi_mode_str);
            }
        }
        
        cJSON *root = cJSON_CreateObject();
        
        if (ret == ESP_OK) {
            cJSON_AddStringToObject(root, "status", "success");
            cJSON_AddStringToObject(root, "message", "Config saved. Rebooting...");
        } else {
            cJSON_AddStringToObject(root, "status", "error");
            cJSON_AddStringToObject(root, "message", "Failed to save config");
        }
        
        const char *json_str = cJSON_Print(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json_str);
        
        free((void *)json_str);
        cJSON_Delete(root);
        
        // Trigger reboot after sending response
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Rebooting in 2 seconds...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
        }
        
        return ESP_OK;
    }
    
    // Missing parameters
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "error");
    cJSON_AddStringToObject(root, "message", "Missing parameters");
    
    const char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    
    free((void *)json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

// API handler: Factory reset (erase NVS and reboot)
static esp_err_t api_factory_reset_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "API: Factory reset requested - erasing NVS");
    
    cJSON *root = cJSON_CreateObject();
    
    // Erase NVS
    esp_err_t ret = nvs_flash_erase();
    
    if (ret == ESP_OK) {
        cJSON_AddStringToObject(root, "status", "success");
        cJSON_AddStringToObject(root, "message", "Factory reset complete. Rebooting...");
        ESP_LOGI(TAG, "NVS erased successfully");
    } else {
        cJSON_AddStringToObject(root, "status", "error");
        cJSON_AddStringToObject(root, "message", "Failed to erase NVS");
        ESP_LOGE(TAG, "NVS erase failed: %s", esp_err_to_name(ret));
    }
    
    const char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    
    free((void *)json_str);
    cJSON_Delete(root);
    
    // Trigger reboot after sending response
    if (ret == ESP_OK) {
        ESP_LOGW(TAG, "Rebooting in 2 seconds...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }
    
    return ESP_OK;
}

// API handler: Simple reboot (no NVS erase)
static esp_err_t api_reboot_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "API: Reboot requested");
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "success");
    cJSON_AddStringToObject(root, "message", "Rebooting...");
    
    const char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    
    free((void *)json_str);
    cJSON_Delete(root);
    
    // Trigger reboot after sending response
    ESP_LOGW(TAG, "Rebooting in 500ms...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    
    return ESP_OK;
}
// Trigger handler - GET to check state, POST to set/clear
// GET /api/trigger?id=xxx - returns trigger state
// POST /api/trigger?id=xxx&state=true/false - sets or clears trigger
static esp_err_t api_trigger_handler(httpd_req_t *req)
{
    char query[128];
    char trigger_id[TRIGGER_ID_MAX_LEN] = "trigger1";  // Default ID
    char state_str[8] = {0};
    
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char id_buf[TRIGGER_ID_MAX_LEN] = {0};
        if (httpd_query_key_value(query, "id", id_buf, sizeof(id_buf)) == ESP_OK && strlen(id_buf) > 0) {
            strncpy(trigger_id, id_buf, TRIGGER_ID_MAX_LEN - 1);
        }
        httpd_query_key_value(query, "state", state_str, sizeof(state_str));
    }
    
    cJSON *root = cJSON_CreateObject();
    
    if (req->method == HTTP_GET) {
        // GET: Check if trigger exists
        bool found = false;
        portENTER_CRITICAL(&trigger_mutex);
        for (int i = 0; i < triggered_count; i++) {
            if (strcmp(triggered_ids[i], trigger_id) == 0) {
                found = true;
                break;
            }
        }
        portEXIT_CRITICAL(&trigger_mutex);
        
        cJSON_AddStringToObject(root, "id", trigger_id);
        cJSON_AddBoolToObject(root, "triggered", found);
        
    } else {
        // POST: Set or clear trigger based on state parameter
        bool set_state = true;  // Default to true (activate)
        
        if (strlen(state_str) > 0) {
            set_state = (strcmp(state_str, "true") == 0 || strcmp(state_str, "1") == 0);
        }
        
        portENTER_CRITICAL(&trigger_mutex);
        
        if (set_state) {
            // Add trigger if not already present
            bool already_exists = false;
            for (int i = 0; i < triggered_count; i++) {
                if (strcmp(triggered_ids[i], trigger_id) == 0) {
                    already_exists = true;
                    break;
                }
            }
            
            if (!already_exists && triggered_count < MAX_TRIGGERS) {
                strncpy(triggered_ids[triggered_count], trigger_id, TRIGGER_ID_MAX_LEN - 1);
                triggered_ids[triggered_count][TRIGGER_ID_MAX_LEN - 1] = '\0';
                triggered_count++;
                ESP_LOGI(TAG, "Trigger activated: %s", trigger_id);
            }
        } else {
            // Remove trigger
            for (int i = 0; i < triggered_count; i++) {
                if (strcmp(triggered_ids[i], trigger_id) == 0) {
                    // Shift remaining triggers
                    for (int j = i; j < triggered_count - 1; j++) {
                        strncpy(triggered_ids[j], triggered_ids[j + 1], TRIGGER_ID_MAX_LEN);
                    }
                    triggered_count--;
                    ESP_LOGI(TAG, "Trigger cleared: %s", trigger_id);
                    break;
                }
            }
        }
        
        portEXIT_CRITICAL(&trigger_mutex);
        
        cJSON_AddStringToObject(root, "status", "ok");
        cJSON_AddStringToObject(root, "id", trigger_id);
        cJSON_AddBoolToObject(root, "triggered", set_state);
    }
    
    const char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");  // Allow cross-origin
    httpd_resp_sendstr(req, json_str);
    
    free((void *)json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

// GPIO read handler - read a GPIO pin state
// GET /api/gpio?pin=X - returns {"level": 0 or 1}
static esp_err_t api_gpio_handler(httpd_req_t *req)
{
    char query[64];
    char pin_str[8] = {0};
    
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "pin", pin_str, sizeof(pin_str));
    }
    
    int pin_num = atoi(pin_str);
    
    // Validate pin number (0-48 for ESP32-S3)
    if (pin_num < 0 || pin_num > 48) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "status", "error");
        cJSON_AddStringToObject(root, "message", "Invalid GPIO pin (0-48)");
        const char *json_str = cJSON_Print(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json_str);
        free((void *)json_str);
        cJSON_Delete(root);
        return ESP_OK;
    }
    
    // Configure pin as input with pull-down (can be changed via query param later)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << pin_num),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    
    // Read the pin
    int level = gpio_get_level(pin_num);
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddNumberToObject(root, "pin", pin_num);
    cJSON_AddNumberToObject(root, "level", level);
    
    const char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    
    free((void *)json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

// ============================================================================
// HTTP API: Chain Execution (POST /api/chain with CSV body)
// ============================================================================

// POST /api/chain - Execute chain from CSV body, return JSON results
static esp_err_t api_chain_http_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "=== API: chain HTTP handler called ===");
    
    // Check if executor is busy
    if (usb_executor_is_busy()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Executor busy\"}");
        return ESP_OK;
    }
    
    // Read CSV body
    size_t content_len = req->content_len;
    if (content_len == 0 || content_len > 32768) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Invalid content length\"}");
        return ESP_OK;
    }
    
    char *csv_data = malloc(content_len + 1);
    if (!csv_data) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Out of memory\"}");
        return ESP_OK;
    }
    
    int received = httpd_req_recv(req, csv_data, content_len);
    if (received != content_len) {
        free(csv_data);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Failed to read body\"}");
        return ESP_OK;
    }
    csv_data[content_len] = '\0';
    
    // Initialize semaphore
    if (!http_api_done_sem) {
        http_api_done_sem = xSemaphoreCreateBinary();
    }
    
    // Reset result storage
    http_api_result_count = 0;
    http_api_final_result = ESP_OK;
    xSemaphoreTake(http_api_done_sem, 0);  // Clear any stale signal
    
    // Submit chain job
    esp_err_t submit_ret = usb_executor_submit_chain(
        csv_data, content_len,
        http_api_result_cb,
        NULL,  // No wait callback for HTTP API
        http_api_done_cb,
        NULL
    );
    free(csv_data);
    
    if (submit_ret != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Failed to submit job\"}");
        return ESP_OK;
    }
    
    // Wait for completion (max 60 seconds)
    if (xSemaphoreTake(http_api_done_sem, pdMS_TO_TICKS(60000)) != pdTRUE) {
        usb_executor_stop();
        httpd_resp_set_status(req, "504 Gateway Timeout");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Execution timeout\"}");
        return ESP_OK;
    }
    
    // Build response JSON
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", http_api_final_result == ESP_OK ? "success" : "error");
    cJSON_AddNumberToObject(root, "entries_executed", http_api_result_count);
    
    cJSON *results = cJSON_CreateArray();
    for (int i = 0; i < http_api_result_count && i < 64; i++) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "i", i);
        cJSON_AddNumberToObject(entry, "t", http_api_results[i].type);
        cJSON_AddNumberToObject(entry, "s", http_api_results[i].status);
        cJSON_AddNumberToObject(entry, "b", http_api_results[i].bytes_received);
        
        if (http_api_results[i].data_len > 0 && http_api_results[i].data_len <= 64) {
            char hex_str[129];
            for (size_t j = 0; j < http_api_results[i].data_len && j < 64; j++) {
                sprintf(hex_str + j * 2, "%02X", http_api_results[i].data[j]);
            }
            hex_str[http_api_results[i].data_len * 2] = '\0';
            cJSON_AddStringToObject(entry, "d", hex_str);
        }
        
        cJSON_AddItemToArray(results, entry);
    }
    cJSON_AddItemToObject(root, "results", results);
    
    const char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    free((void *)json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

// POST /api/bruteforce - Execute bruteforce from JSON body
static esp_err_t api_bruteforce_http_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "=== API: bruteforce HTTP handler called ===");
    
    // Check if executor is busy
    if (usb_executor_is_busy()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Executor busy\"}");
        return ESP_OK;
    }
    
    // Read CSV body (same format as UI export)
    size_t content_len = req->content_len;
    if (content_len == 0 || content_len > 4096) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Invalid content length\"}");
        return ESP_OK;
    }
    
    char *body = malloc(content_len + 1);
    if (!body) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Out of memory\"}");
        return ESP_OK;
    }
    
    int received = httpd_req_recv(req, body, content_len);
    if (received != content_len) {
        free(body);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Failed to read body\"}");
        return ESP_OK;
    }
    body[content_len] = '\0';
    
    // Parse CSV format: field,iterate,start,end,exclude
    usb_job_bruteforce_t params = {0};
    params.delay_ms = 50;  // Default
    
    // Field name to struct pointer mapping
    struct { const char *name; usb_bf_field_t *field; } field_map[] = {
        {"bmRequestType", &params.bmRequestType},
        {"bRequest", &params.bRequest},
        {"wValueHi", &params.wValueHi},
        {"wValueLo", &params.wValueLo},
        {"wIndexHi", &params.wIndexHi},
        {"wIndexLo", &params.wIndexLo},
        {"wLength", &params.wLength},
        {"packetSize", &params.packetSize}
    };
    
    char *line = strtok(body, "\n");
    while (line) {
        // Skip header line
        if (strncmp(line, "field,", 6) == 0) {
            line = strtok(NULL, "\n");
            continue;
        }
        
        // Parse CSV line: field,iterate,start,end,exclude
        char field_name[32] = {0};
        char iterate_str[8] = {0};
        char start_str[16] = {0};
        char end_str[16] = {0};
        
        // Simple CSV parse (handles quoted fields too)
        char *p = line;
        char *dst = field_name;
        int col = 0;
        while (*p && *p != '\r') {
            if (*p == ',') {
                col++;
                p++;
                if (col == 1) dst = iterate_str;
                else if (col == 2) dst = start_str;
                else if (col == 3) dst = end_str;
                else dst = NULL;
                continue;
            }
            if (*p == '"') { p++; continue; }  // Skip quotes
            if (dst && (dst - (col == 0 ? field_name : col == 1 ? iterate_str : col == 2 ? start_str : end_str)) < 15) {
                *dst++ = *p;
            }
            p++;
        }
        
        // Handle special fields
        if (strcmp(field_name, "delay") == 0) {
            params.delay_ms = (uint32_t)atoi(iterate_str);
        } else if (strcmp(field_name, "dataMode") == 0 || strcmp(field_name, "dataBytes") == 0) {
            // Skip for now (not implemented in bruteforce executor yet)
        } else {
            // Match field name to struct
            for (int i = 0; i < 8; i++) {
                if (strcmp(field_name, field_map[i].name) == 0) {
                    field_map[i].field->iterate = (iterate_str[0] == '1');
                    field_map[i].field->start = (uint16_t)strtol(start_str, NULL, 0);
                    field_map[i].field->end = (uint16_t)strtol(end_str, NULL, 0);
                    if (!field_map[i].field->iterate) {
                        field_map[i].field->end = field_map[i].field->start;
                    }
                    break;
                }
            }
        }
        
        line = strtok(NULL, "\n");
    }
    free(body);
    
    // Initialize semaphore
    if (!http_api_done_sem) {
        http_api_done_sem = xSemaphoreCreateBinary();
    }
    
    // Reset result storage
    http_api_result_count = 0;
    http_api_final_result = ESP_OK;
    xSemaphoreTake(http_api_done_sem, 0);
    
    // Submit bruteforce job
    esp_err_t submit_ret = usb_executor_submit_bruteforce(&params, http_api_result_cb, http_api_done_cb, NULL);
    
    if (submit_ret != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Failed to submit job\"}");
        return ESP_OK;
    }
    
    // Wait for completion (max 5 minutes for bruteforce)
    if (xSemaphoreTake(http_api_done_sem, pdMS_TO_TICKS(300000)) != pdTRUE) {
        usb_executor_stop();
        httpd_resp_set_status(req, "504 Gateway Timeout");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Execution timeout\"}");
        return ESP_OK;
    }
    
    // Build response JSON (summary only, not all results)
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", http_api_final_result == ESP_OK ? "success" : "error");
    cJSON_AddNumberToObject(root, "iterations", http_api_result_count);
    
    // Count successes
    int success_count = 0;
    for (int i = 0; i < http_api_result_count; i++) {
        if (http_api_results[i].status == 0 && http_api_results[i].bytes_received > 0) {
            success_count++;
        }
    }
    cJSON_AddNumberToObject(root, "successes", success_count);
    
    const char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    free((void *)json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

// ============================================================================
// Native Chain Execution API (WebSocket)
// ============================================================================

// WebSocket client for streaming results
static httpd_handle_t chain_ws_server = NULL;
static int chain_ws_fd = -1;

// Helper to send WebSocket message
static void chain_ws_send_json(cJSON *json) {
    if (chain_ws_fd < 0 || chain_ws_server == NULL) return;
    const char *json_str = cJSON_PrintUnformatted(json);
    if (json_str) {
        httpd_ws_frame_t pkt = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)json_str,
            .len = strlen(json_str)
        };
        httpd_ws_send_frame_async(chain_ws_server, chain_ws_fd, &pkt);
        free((void *)json_str);
    }
}

// Callback to stream results via WebSocket
static void chain_ws_callback(int index, const chain_result_t *result, void *user_data) {
    // Update USB stats for all USB transfer types
    if (result->type <= CHAIN_TYPE_INTERRUPT_OUT) {
        update_usb_stats(result->bytes_received, 8);  // 8 bytes for SETUP packet
        ESP_LOGD(TAG, "Stats update: type=%d rx=%d", result->type, result->bytes_received);
    }
    
    if (chain_ws_fd < 0 || chain_ws_server == NULL) {
        ESP_LOGW(TAG, "WS callback: no connection (fd=%d)", chain_ws_fd);
        return;
    }
    ESP_LOGI(TAG, "WS callback: index=%d status=%d bytes=%d data_len=%d", 
             index, result->status, result->bytes_received, result->data_len);
    
    // Check if this is a button wait result - send waiting message
    if (result->type == CHAIN_TYPE_WAIT_BUTTON && result->status == 0) {
        // Button was pressed, normal result
    }
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "i", index);
    cJSON_AddNumberToObject(root, "t", result->type);
    cJSON_AddNumberToObject(root, "s", result->status);
    cJSON_AddNumberToObject(root, "b", result->bytes_received);
    
    // Include USB params (for bruteforce display)
    if (result->bmRequestType || result->bRequest || result->wValue || result->wIndex) {
        cJSON_AddNumberToObject(root, "bmRT", result->bmRequestType);
        cJSON_AddNumberToObject(root, "bReq", result->bRequest);
        cJSON_AddNumberToObject(root, "wVal", result->wValue);
        cJSON_AddNumberToObject(root, "wIdx", result->wIndex);
        cJSON_AddNumberToObject(root, "wLen", result->wLength);
        cJSON_AddNumberToObject(root, "pkt", result->packetSize);
    }
    
    if (result->data_len > 0) {
        char *hex_str = malloc(result->data_len * 2 + 1);
        if (hex_str) {
            for (size_t i = 0; i < result->data_len && i < 256; i++) {
                sprintf(hex_str + i * 2, "%02X", result->data[i]);
            }
            hex_str[result->data_len * 2] = '\0';
            cJSON_AddStringToObject(root, "d", hex_str);
            ESP_LOGI(TAG, "WS callback: sending %d bytes hex: %.32s...", result->data_len, hex_str);
            free(hex_str);
        }
    } else {
        ESP_LOGW(TAG, "WS callback: data_len is 0, no hex data to send");
    }
    
    chain_ws_send_json(root);
    cJSON_Delete(root);
}

// Callback for wait notifications (button/webhook)
static void chain_wait_notify(int index, chain_entry_type_t type, const char *info, void *user_data) {
    if (chain_ws_fd < 0 || chain_ws_server == NULL) return;
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "i", index);
    
    if (type == CHAIN_TYPE_WAIT_BUTTON) {
        cJSON_AddStringToObject(root, "waiting", "button");
        cJSON_AddStringToObject(root, "label", info ? info : "Press Continue");
    } else if (type == CHAIN_TYPE_WAIT_WEBHOOK) {
        cJSON_AddStringToObject(root, "waiting", "webhook");
        cJSON_AddStringToObject(root, "trigger_id", info ? info : "");
    }
    
    chain_ws_send_json(root);
    cJSON_Delete(root);
}

// Callback when job completes (called from Core 1)
static void chain_done_notify(esp_err_t result, void *user_data) {
    if (chain_ws_fd < 0 || chain_ws_server == NULL) {
        ESP_LOGW(TAG, "Done notify: no connection (fd=%d)", chain_ws_fd);
        return;
    }
    ESP_LOGI(TAG, "Done notify: result=%d", result);
    
    cJSON *done = cJSON_CreateObject();
    cJSON_AddStringToObject(done, "status", result == ESP_OK ? "done" : "error");
    chain_ws_send_json(done);
    cJSON_Delete(done);
    
    // Clear WebSocket context
    chain_ws_fd = -1;
}

// Execute a single USB request from JSON params - build CSV and run as chain
static void ws_execute_single(httpd_req_t *req, cJSON *json) {
    // Build CSV line from JSON params (single request = chain with 1 entry)
    char csv[256];
    int len = 0;
    
    cJSON *item;
    const char *bmReq = "0x80", *bReq = "0x06", *wVal = "0x0100", *wIdx = "0x0000";
    int wLen = 18, pktSize = 8, devAddr = 0, dataEp = 0;
    
    if ((item = cJSON_GetObjectItem(json, "bmRequestType")) && cJSON_IsString(item)) bmReq = item->valuestring;
    if ((item = cJSON_GetObjectItem(json, "bRequest")) && cJSON_IsString(item)) bReq = item->valuestring;
    if ((item = cJSON_GetObjectItem(json, "wValue")) && cJSON_IsString(item)) wVal = item->valuestring;
    if ((item = cJSON_GetObjectItem(json, "wIndex")) && cJSON_IsString(item)) wIdx = item->valuestring;
    if ((item = cJSON_GetObjectItem(json, "wLength"))) wLen = cJSON_IsNumber(item) ? item->valueint : 18;
    if ((item = cJSON_GetObjectItem(json, "packetSize"))) pktSize = cJSON_IsNumber(item) ? item->valueint : 8;
    if ((item = cJSON_GetObjectItem(json, "deviceAddr"))) devAddr = cJSON_IsNumber(item) ? item->valueint : 0;
    if ((item = cJSON_GetObjectItem(json, "dataStageEp"))) dataEp = cJSON_IsNumber(item) ? item->valueint : 0;
    
    // Build CSV: control,bmReq,bReq,wVal,wIdx,wLen,pktSize,devAddr,dataEp
    len = snprintf(csv, sizeof(csv), "control,%s,%s,%s,%s,%d,%d,%d,%d",
                   bmReq, bReq, wVal, wIdx, wLen, pktSize, devAddr, dataEp);
    
    // Store WebSocket context for callbacks
    chain_ws_server = req->handle;
    chain_ws_fd = httpd_req_to_sockfd(req);
    
    // Copy CSV to heap (executor will free it)
    char *csv_copy = strdup(csv);
    if (!csv_copy) {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "status", "error");
        cJSON_AddStringToObject(err, "message", "Out of memory");
        chain_ws_send_json(err);
        cJSON_Delete(err);
        chain_ws_fd = -1;
        return;
    }
    
    ESP_LOGI(TAG, "Single request as chain: %s", csv);
    
    // Submit as chain - truly unified path!
    esp_err_t ret = usb_executor_submit_chain(csv_copy, len, chain_ws_callback, chain_wait_notify, chain_done_notify, NULL);
    
    if (ret != ESP_OK) {
        free(csv_copy);
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "status", "error");
        cJSON_AddStringToObject(err, "message", "Executor busy");
        chain_ws_send_json(err);
        cJSON_Delete(err);
        chain_ws_fd = -1;
    }
}

// Execute bruteforce from JSON config - submit to executor
static void ws_execute_bruteforce(httpd_req_t *req, cJSON *json) {
    usb_job_bruteforce_t params = {0};
    
    // Parse field configurations
    const char *field_names[] = {"bmRequestType", "bRequest", "wValueHi", "wValueLo", 
                                  "wIndexHi", "wIndexLo", "wLength", "packetSize"};
    usb_bf_field_t *bf_fields[] = {
        &params.bmRequestType, &params.bRequest, &params.wValueHi, &params.wValueLo,
        &params.wIndexHi, &params.wIndexLo, &params.wLength, &params.packetSize
    };
    
    for (int i = 0; i < 8; i++) {
        cJSON *field = cJSON_GetObjectItem(json, field_names[i]);
        if (field) {
            cJSON *start = cJSON_GetObjectItem(field, "start");
            cJSON *end = cJSON_GetObjectItem(field, "end");
            cJSON *iterate = cJSON_GetObjectItem(field, "iterate");
            bf_fields[i]->start = start ? (uint16_t)start->valueint : 0;
            bf_fields[i]->end = end ? (uint16_t)end->valueint : bf_fields[i]->start;
            bf_fields[i]->iterate = iterate ? cJSON_IsTrue(iterate) : false;
        }
    }
    
    cJSON *delay_item = cJSON_GetObjectItem(json, "delay");
    params.delay_ms = delay_item ? (uint32_t)delay_item->valueint : 50;
    
    // Count total iterations for logging
    uint64_t total = 1;
    for (int i = 0; i < 8; i++) {
        if (bf_fields[i]->iterate) {
            total *= (bf_fields[i]->end - bf_fields[i]->start + 1);
        }
    }
    ESP_LOGI(TAG, "Bruteforce: %"PRIu64" iterations, %"PRIu32" ms delay", total, params.delay_ms);
    
    // Store WebSocket context for callbacks
    chain_ws_server = req->handle;
    chain_ws_fd = httpd_req_to_sockfd(req);
    
    // Submit to executor on Core 1
    esp_err_t ret = usb_executor_submit_bruteforce(&params, chain_ws_callback, chain_done_notify, NULL);
    
    if (ret != ESP_OK) {
        // Failed to submit - send error immediately
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "status", "error");
        cJSON_AddStringToObject(err, "message", "Executor busy");
        chain_ws_send_json(err);
        cJSON_Delete(err);
        chain_ws_fd = -1;
    }
}

// WebSocket handler: /ws/usb
// Unified handler for single requests, bruteforce, and chains
static esp_err_t api_chain_ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "USB WebSocket connected");
        return ESP_OK;
    }
    
    httpd_ws_frame_t pkt = {0};
    pkt.type = HTTPD_WS_TYPE_TEXT;
    
    // Get frame length
    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);
    if (ret != ESP_OK) return ret;
    
    if (pkt.len == 0) return ESP_OK;
    
    // Allocate and receive
    pkt.payload = malloc(pkt.len + 1);
    if (!pkt.payload) return ESP_ERR_NO_MEM;
    
    ret = httpd_ws_recv_frame(req, &pkt, pkt.len);
    if (ret != ESP_OK) {
        free(pkt.payload);
        return ret;
    }
    pkt.payload[pkt.len] = '\0';
    
    char *payload = (char*)pkt.payload;
    
    // Check for stop command
    if (pkt.len < 10 && strstr(payload, "stop") != NULL) {
        usb_executor_stop();  // Stops both chain and bruteforce
        free(pkt.payload);
        return ESP_OK;
    }
    
    // Check if JSON command (starts with '{')
    if (payload[0] == '{') {
        cJSON *json = cJSON_Parse(payload);
        free(pkt.payload);
        
        if (!json) {
            ESP_LOGW(TAG, "Invalid JSON");
            return ESP_OK;
        }
        
        cJSON *cmd = cJSON_GetObjectItem(json, "cmd");
        if (cmd && cJSON_IsString(cmd)) {
            if (strcmp(cmd->valuestring, "single") == 0) {
                ESP_LOGI(TAG, "Executing single request");
                ws_execute_single(req, json);
            } else if (strcmp(cmd->valuestring, "bruteforce") == 0) {
                ESP_LOGI(TAG, "Executing bruteforce");
                ws_execute_bruteforce(req, json);
            } else if (strcmp(cmd->valuestring, "continue") == 0) {
                // User clicked continue button in modal
                ESP_LOGI(TAG, "Button continue received");
                chain_button_continue();
                
                // Send ack
                cJSON *ack = cJSON_CreateObject();
                cJSON_AddStringToObject(ack, "status", "ok");
                const char *ack_str = cJSON_PrintUnformatted(ack);
                if (ack_str) {
                    httpd_ws_frame_t ack_pkt = {
                        .type = HTTPD_WS_TYPE_TEXT,
                        .payload = (uint8_t *)ack_str,
                        .len = strlen(ack_str)
                    };
                    httpd_ws_send_frame(req, &ack_pkt);
                    free((void *)ack_str);
                }
                cJSON_Delete(ack);
            } else if (strcmp(cmd->valuestring, "webhook") == 0) {
                // External webhook trigger
                cJSON *trigger_id = cJSON_GetObjectItem(json, "trigger_id");
                const char *tid = (trigger_id && cJSON_IsString(trigger_id)) ? trigger_id->valuestring : "";
                ESP_LOGI(TAG, "Webhook trigger: %s", tid);
                chain_webhook_trigger(tid);
                
                // Send ack
                cJSON *ack = cJSON_CreateObject();
                cJSON_AddStringToObject(ack, "status", "ok");
                const char *ack_str = cJSON_PrintUnformatted(ack);
                if (ack_str) {
                    httpd_ws_frame_t ack_pkt = {
                        .type = HTTPD_WS_TYPE_TEXT,
                        .payload = (uint8_t *)ack_str,
                        .len = strlen(ack_str)
                    };
                    httpd_ws_send_frame(req, &ack_pkt);
                    free((void *)ack_str);
                }
                cJSON_Delete(ack);
            }
        }
        cJSON_Delete(json);
        return ESP_OK;
    }
    
    // Otherwise treat as CSV chain - submit to executor on Core 1
    chain_ws_server = req->handle;
    chain_ws_fd = httpd_req_to_sockfd(req);
    
    ESP_LOGI(TAG, "Submitting chain to executor (%d bytes)", (int)pkt.len);
    esp_err_t submit_ret = usb_executor_submit_chain(
        payload, pkt.len,
        chain_ws_callback,
        chain_wait_notify,
        chain_done_notify,
        NULL
    );
    
    free(pkt.payload);
    
    if (submit_ret != ESP_OK) {
        // Failed to submit - send error immediately
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "status", "error");
        cJSON_AddStringToObject(err, "message", "Executor busy");
        chain_ws_send_json(err);
        cJSON_Delete(err);
        chain_ws_fd = -1;
    }
    
    // Job runs asynchronously, done_notify will send completion
    return ESP_OK;
}

// POST /api/chain/stop - Stop running chain
static esp_err_t api_chain_stop_handler(httpd_req_t *req) {
    chain_stop();
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "success");
    
    const char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    free((void *)json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

// Root handler - serve embedded HTML
static esp_err_t root_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "=== ROOT: Serving index.html ===");
    const uint32_t index_html_len = index_html_end - index_html_start;
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "identity");
    esp_err_t ret = httpd_resp_send(req, (const char *)index_html_start, index_html_len);
    
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send HTML: %s", esp_err_to_name(ret));
    }
    return ret;
}

// JavaScript handler - serve embedded app.js
static esp_err_t app_js_handler(httpd_req_t *req)
{
    const uint32_t app_js_len = app_js_end - app_js_start;
    
    httpd_resp_set_type(req, "application/javascript");
    esp_err_t ret = httpd_resp_send(req, (const char *)app_js_start, app_js_len);
    
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send app.js: %s", esp_err_to_name(ret));
    }
    return ret;
}

// Logo handler - serve embedded logo.svg
static esp_err_t logo_svg_handler(httpd_req_t *req)
{
    const uint32_t logo_svg_len = logo_svg_end - logo_svg_start;
    
    httpd_resp_set_type(req, "image/svg+xml");
    esp_err_t ret = httpd_resp_send(req, (const char *)logo_svg_start, logo_svg_len);
    
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send logo.svg: %s", esp_err_to_name(ret));
    }
    return ret;
}

// App logo handler - serve embedded applogo.png
static esp_err_t applogo_png_handler(httpd_req_t *req)
{
    const uint32_t applogo_png_len = applogo_png_end - applogo_png_start;
    
    httpd_resp_set_type(req, "image/png");
    esp_err_t ret = httpd_resp_send(req, (const char *)applogo_png_start, applogo_png_len);
    
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send applogo.png: %s", esp_err_to_name(ret));
    }
    return ret;
}

// App text handler - serve embedded apptext.svg
static esp_err_t apptext_svg_handler(httpd_req_t *req)
{
    const uint32_t apptext_svg_len = apptext_svg_end - apptext_svg_start;
    
    httpd_resp_set_type(req, "image/svg+xml");
    esp_err_t ret = httpd_resp_send(req, (const char *)apptext_svg_start, apptext_svg_len);
    
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send apptext.svg: %s", esp_err_to_name(ret));
    }
    return ret;
}

// Favicon handler - serve embedded favicon.ico
static esp_err_t favicon_ico_handler(httpd_req_t *req)
{
    const uint32_t favicon_ico_len = favicon_ico_end - favicon_ico_start;
    
    httpd_resp_set_type(req, "image/x-icon");
    esp_err_t ret = httpd_resp_send(req, (const char *)favicon_ico_start, favicon_ico_len);
    
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send favicon.ico: %s", esp_err_to_name(ret));
    }
    return ret;
}

// API documentation handler - serve embedded api.html
static esp_err_t api_html_handler(httpd_req_t *req)
{
    const uint32_t api_html_len = api_html_end - api_html_start;
    
    httpd_resp_set_type(req, "text/html");
    esp_err_t ret = httpd_resp_send(req, (const char *)api_html_start, api_html_len);
    
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send api.html: %s", esp_err_to_name(ret));
    }
    return ret;
}

// OpenAPI spec handler - serve embedded openapi.json
static esp_err_t openapi_json_handler(httpd_req_t *req)
{
    const uint32_t openapi_json_len = openapi_json_end - openapi_json_start;
    
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, (const char *)openapi_json_start, openapi_json_len);
    
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send openapi.json: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t web_interface_start(void)
{
    // HTTP server configuration - runs on Core 0 (networking core)
    // USB operations run on Core 1 (dedicated USB core) via usb_malformed.c
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 40;
    config.lru_purge_enable = true;  // Enable connection purging
    config.recv_wait_timeout = 10;    // 10 second timeout
    config.send_wait_timeout = 10;    // 10 second timeout
    config.stack_size = 8192;         // Increase from default 4096 to prevent stack overflow
    config.core_id = 0;               // Pin HTTP server to Core 0 (networking core)
    
    ESP_LOGI(TAG, "Starting web server on Core 0 (networking core), port %d", config.server_port);
    
    if (httpd_start(&server, &config) == ESP_OK) {
        // Register URI handlers
        httpd_uri_t root_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_handler
        };
        httpd_register_uri_handler(server, &root_uri);
        
        httpd_uri_t app_js_uri = {
            .uri = "/app.js",
            .method = HTTP_GET,
            .handler = app_js_handler
        };
        httpd_register_uri_handler(server, &app_js_uri);
        
        httpd_uri_t logo_svg_uri = {
            .uri = "/logo.svg",
            .method = HTTP_GET,
            .handler = logo_svg_handler
        };
        httpd_register_uri_handler(server, &logo_svg_uri);
        
        httpd_uri_t applogo_png_uri = {
            .uri = "/applogo.png",
            .method = HTTP_GET,
            .handler = applogo_png_handler
        };
        httpd_register_uri_handler(server, &applogo_png_uri);
        
        httpd_uri_t apptext_svg_uri = {
            .uri = "/apptext.svg",
            .method = HTTP_GET,
            .handler = apptext_svg_handler
        };
        httpd_register_uri_handler(server, &apptext_svg_uri);
        
        httpd_uri_t favicon_ico_uri = {
            .uri = "/favicon.ico",
            .method = HTTP_GET,
            .handler = favicon_ico_handler
        };
        httpd_register_uri_handler(server, &favicon_ico_uri);
        
        httpd_uri_t api_docs_uri = {
            .uri = "/api",
            .method = HTTP_GET,
            .handler = api_html_handler
        };
        httpd_register_uri_handler(server, &api_docs_uri);
        
        httpd_uri_t openapi_spec_uri = {
            .uri = "/openapi.json",
            .method = HTTP_GET,
            .handler = openapi_json_handler
        };
        httpd_register_uri_handler(server, &openapi_spec_uri);
        
        httpd_uri_t api_single_request_uri = {
            .uri = "/api/single_request",
            .method = HTTP_POST,
            .handler = api_single_request_handler
        };
        httpd_register_uri_handler(server, &api_single_request_uri);
        
        httpd_uri_t api_status_uri = {
            .uri = "/api/status",
            .method = HTTP_GET,
            .handler = api_status_handler
        };
        httpd_register_uri_handler(server, &api_status_uri);
        
        httpd_uri_t api_version_uri = {
            .uri = "/api/version",
            .method = HTTP_GET,
            .handler = api_version_handler
        };
        httpd_register_uri_handler(server, &api_version_uri);
        
        httpd_uri_t api_device_info_uri = {
            .uri = "/api/device_info",
            .method = HTTP_GET,
            .handler = api_device_info_handler
        };
        httpd_register_uri_handler(server, &api_device_info_uri);
        
        httpd_uri_t api_reset_uri = {
            .uri = "/api/reset",
            .method = HTTP_POST,
            .handler = api_reset_handler
        };
        httpd_register_uri_handler(server, &api_reset_uri);
        
        httpd_uri_t api_save_config_uri = {
            .uri = "/api/save_config",
            .method = HTTP_POST,
            .handler = api_save_config_handler
        };
        httpd_register_uri_handler(server, &api_save_config_uri);
        
        httpd_uri_t api_factory_reset_uri = {
            .uri = "/api/factory_reset",
            .method = HTTP_POST,
            .handler = api_factory_reset_handler
        };
        httpd_register_uri_handler(server, &api_factory_reset_uri);
        
        httpd_uri_t api_reboot_uri = {
            .uri = "/api/reboot",
            .method = HTTP_POST,
            .handler = api_reboot_handler
        };
        httpd_register_uri_handler(server, &api_reboot_uri);
        
        httpd_uri_t api_stats_uri = {
            .uri = "/api/stats",
            .method = HTTP_GET,
            .handler = api_stats_handler
        };
        httpd_register_uri_handler(server, &api_stats_uri);
        
        httpd_uri_t api_wifi_config_get_uri = {
            .uri = "/api/wifi_config",
            .method = HTTP_GET,
            .handler = api_wifi_config_handler
        };
        httpd_register_uri_handler(server, &api_wifi_config_get_uri);
        
        httpd_uri_t api_wifi_config_post_uri = {
            .uri = "/api/wifi_config",
            .method = HTTP_POST,
            .handler = api_wifi_config_handler
        };
        httpd_register_uri_handler(server, &api_wifi_config_post_uri);

        httpd_uri_t api_trigger_get_uri = {
            .uri = "/api/trigger",
            .method = HTTP_GET,
            .handler = api_trigger_handler
        };
        httpd_register_uri_handler(server, &api_trigger_get_uri);
        
        httpd_uri_t api_trigger_post_uri = {
            .uri = "/api/trigger",
            .method = HTTP_POST,
            .handler = api_trigger_handler
        };
        httpd_register_uri_handler(server, &api_trigger_post_uri);

        httpd_uri_t api_gpio_get_uri = {
            .uri = "/api/gpio",
            .method = HTTP_GET,
            .handler = api_gpio_handler
        };
        httpd_register_uri_handler(server, &api_gpio_get_uri);
        
        httpd_uri_t api_gpio_post_uri = {
            .uri = "/api/gpio",
            .method = HTTP_POST,
            .handler = api_gpio_handler
        };
        httpd_register_uri_handler(server, &api_gpio_post_uri);
        
        // HTTP API for chain execution (POST CSV body)
        httpd_uri_t api_chain_http_uri = {
            .uri = "/api/chain",
            .method = HTTP_POST,
            .handler = api_chain_http_handler
        };
        httpd_register_uri_handler(server, &api_chain_http_uri);
        
        // HTTP API for bruteforce execution (POST JSON body)
        httpd_uri_t api_bruteforce_http_uri = {
            .uri = "/api/bruteforce",
            .method = HTTP_POST,
            .handler = api_bruteforce_http_handler
        };
        httpd_register_uri_handler(server, &api_bruteforce_http_uri);

        // Native Chain Execution: WebSocket sends CSV, receives results
        httpd_uri_t chain_ws_uri = {
            .uri = "/ws/chain",
            .method = HTTP_GET,
            .handler = api_chain_ws_handler,
            .is_websocket = true
        };
        httpd_register_uri_handler(server, &chain_ws_uri);

        httpd_uri_t chain_stop_uri = {
            .uri = "/api/chain/stop",
            .method = HTTP_POST,
            .handler = api_chain_stop_handler
        };
        httpd_register_uri_handler(server, &chain_stop_uri);

        // Initialize chain engine
        chain_engine_init();

        return ESP_OK;
    }
    
    ESP_LOGE(TAG, "Failed to start web server");
    return ESP_FAIL;
}

esp_err_t web_interface_stop(void)
{
    if (server) {
        httpd_stop(server);
        server = NULL;
    }
    
    return ESP_OK;
}

