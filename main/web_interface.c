/*
 * USBane - Web Interface
 * Allows real-time parameter adjustment via web browser
 */

#include "web_interface.h"
#include "usb_malformed.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include <string.h>

// Web Interface Constants
#define HTTP_QUERY_MAX_LEN          1024
#define HTTP_DATA_BYTES_MAX_LEN     512
#define HTTP_CUSTOM_DATA_MAX_LEN    248     // Same as USB_MAX_EXTRA_DATA
#define HTTP_RESPONSE_BUFFER_SIZE   512
#define HTTP_HEX_STR_MAX_LEN        384     // 128 bytes * 3
#define HTTP_ASCII_STR_MAX_LEN      256
#define HTTP_RESPONSE_MAX_BYTES     128

static const char *TAG = "WEB_UI";
static httpd_handle_t server = NULL;

// Global flag set by main.c when device connects/reconnects
bool usb_needs_reset = true;  // Start true for first connection

// Webhook trigger storage for chain waitfor actions
#define MAX_TRIGGERS 16
#define TRIGGER_ID_MAX_LEN 32
static char triggered_ids[MAX_TRIGGERS][TRIGGER_ID_MAX_LEN] = {0};
static int triggered_count = 0;
static portMUX_TYPE trigger_mutex = portMUX_INITIALIZER_UNLOCKED;

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
static void update_usb_stats(size_t bytes_rx, size_t bytes_tx);
static void update_cpu_load(void);

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

// API handler: Send USB request
static esp_err_t api_send_request_handler(httpd_req_t *req)
{
    static char query[HTTP_QUERY_MAX_LEN];
    static char dataBytes_str[HTTP_DATA_BYTES_MAX_LEN];
    static uint8_t customData[HTTP_CUSTOM_DATA_MAX_LEN];
    
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char bmRequestType_str[16], bRequest_str[16], wValue_str[16], wIndex_str[16], wLength_str[16];
        char packetSize_str[16], maxRetries_str[16], dataMode_str[16];
        uint8_t bmRequestType = 0x80;
        uint8_t bRequest = 0x06;
        uint16_t wValue = 0x0100;
        uint16_t wIndex = 0x0000;
        uint16_t wLength = 18;
        size_t packetSize = 8;
        int maxRetries = -1;
        bool hasCustomData = false;
        size_t customDataLen = 0;
        bool dataMode_append = false;  // Default: separate DATA OUT stage
        
        if (httpd_query_key_value(query, "bmRequestType", bmRequestType_str, sizeof(bmRequestType_str)) == ESP_OK) {
            bmRequestType = (uint8_t)strtol(bmRequestType_str, NULL, 0);
        }
        if (httpd_query_key_value(query, "bRequest", bRequest_str, sizeof(bRequest_str)) == ESP_OK) {
            bRequest = (uint8_t)strtol(bRequest_str, NULL, 0);
        }
        if (httpd_query_key_value(query, "wValue", wValue_str, sizeof(wValue_str)) == ESP_OK) {
            wValue = (uint16_t)strtol(wValue_str, NULL, 0);
        }
        if (httpd_query_key_value(query, "wIndex", wIndex_str, sizeof(wIndex_str)) == ESP_OK) {
            wIndex = (uint16_t)strtol(wIndex_str, NULL, 0);
        }
        if (httpd_query_key_value(query, "wLength", wLength_str, sizeof(wLength_str)) == ESP_OK) {
            wLength = (uint16_t)atoi(wLength_str);
        }
        if (httpd_query_key_value(query, "packetSize", packetSize_str, sizeof(packetSize_str)) == ESP_OK) {
            packetSize = (size_t)atoi(packetSize_str);
            // Validate: packet_size 0 would hang the USB controller
            if (packetSize <= 0) {
                cJSON *root = cJSON_CreateObject();
                cJSON_AddStringToObject(root, "status", "failed");
                cJSON_AddStringToObject(root, "data", "ERROR: packetSize <= 0 is invalid");
                cJSON_AddStringToObject(root, "ascii", "");
                cJSON_AddNumberToObject(root, "bytes_received", 0);
                const char *json_str = cJSON_Print(root);
                httpd_resp_set_type(req, "application/json");
                httpd_resp_sendstr(req, json_str);
                free((void *)json_str);
                cJSON_Delete(root);
                return ESP_OK;
            }
        }
        if (httpd_query_key_value(query, "maxRetries", maxRetries_str, sizeof(maxRetries_str)) == ESP_OK) {
            maxRetries = atoi(maxRetries_str);
        }
        if (httpd_query_key_value(query, "dataMode", dataMode_str, sizeof(dataMode_str)) == ESP_OK) {
            dataMode_append = (strcmp(dataMode_str, "append") == 0);
        }
        
        // Parse custom DATA bytes (hex string like "41 42 43 AA BB CC")
        if (httpd_query_key_value(query, "dataBytes", dataBytes_str, sizeof(dataBytes_str)) == ESP_OK) {
            if (strlen(dataBytes_str) > 0) {
                hasCustomData = true;
                char *token = strtok(dataBytes_str, " ,");
                while (token != NULL && customDataLen < sizeof(customData)) {
                    customData[customDataLen++] = (uint8_t)strtol(token, NULL, 16);
                    token = strtok(NULL, " ,");
                }
                ESP_LOGI(TAG, "Custom DATA: %d bytes", customDataLen);
            }
        }
        
        ESP_LOGI(TAG, "API: USB Request - bmRequestType=0x%02x, bRequest=0x%02x, wValue=0x%04x, wIndex=0x%04x, wLength=%d, packetSize=%d, maxRetries=%d", 
                 bmRequestType, bRequest, wValue, wIndex, wLength, packetSize, maxRetries);
        
        // Send USB reset if device was recently (re)connected
        // Flag is set by main.c which monitors connection status continuously
        extern bool usb_needs_reset;
        if (usb_needs_reset && usb_is_device_connected()) {
            ESP_LOGI(TAG, "Sending USB reset (device recently connected)...");
            usb_send_reset();
            usb_needs_reset = false;
        }
        
        // Create packet config
        usb_packet_config_t config = usb_packet_config_default();
        config.bmRequestType = bmRequestType;
        config.bRequest = bRequest;
        config.wValue = wValue;
        config.wIndex = wIndex;
        config.wLength = wLength;
        config.packet_size = packetSize;
        config.max_nak_retries = maxRetries;
        
        // Allocate response buffer (static to avoid stack overflow)
        static uint8_t response_buffer[HTTP_RESPONSE_BUFFER_SIZE];
        size_t bytes_received = 0;
        
        config.response_buffer = response_buffer;
        config.response_buffer_size = sizeof(response_buffer);
        config.bytes_received = &bytes_received;
        
        // Handle custom data based on mode
        if (hasCustomData && customDataLen > 0) {
            if (dataMode_append) {
                // Mode: Append to SETUP (oversized packet attack)
                // Increase packet size to include custom data
                packetSize = USB_SETUP_PACKET_SIZE + customDataLen;
                if (packetSize > USB_MAX_PACKET_SIZE) {
                    packetSize = USB_MAX_PACKET_SIZE;
                    customDataLen = USB_MAX_PACKET_SIZE - USB_SETUP_PACKET_SIZE;
                }
                memcpy(config.extra_data, customData, customDataLen);
                config.packet_size = packetSize;
                ESP_LOGI(TAG, "Mode: APPEND - Oversized SETUP packet: %d bytes (8 + %d)", packetSize, customDataLen);
            } else {
                // Mode: Separate DATA OUT stage (normal USB protocol)
                // Keep packet size at 8, send custom data as separate DATA OUT transaction
                packetSize = USB_SETUP_PACKET_SIZE;
                memcpy(config.extra_data, customData, customDataLen);
                config.packet_size = packetSize;
                // Update wLength to match actual data being sent
                if (wLength < customDataLen) {
                    wLength = customDataLen;
                    config.wLength = wLength;
                }
                ESP_LOGI(TAG, "Mode: SEPARATE - DATA OUT stage: %d bytes (wLength=%d)", customDataLen, wLength);
            }
        } else if (packetSize > USB_SETUP_PACKET_SIZE) {
            // Oversized packet without custom data - use auto-pattern
            size_t extraDataLen = packetSize - USB_SETUP_PACKET_SIZE;
            if (extraDataLen > USB_MAX_EXTRA_DATA) {
                extraDataLen = USB_MAX_EXTRA_DATA;
            }
            for (size_t i = 0; i < extraDataLen; i++) {
                config.extra_data[i] = 0xAA + (i % 8);
            }
            ESP_LOGI(TAG, "Auto-pattern oversized SETUP: %d bytes", packetSize);
        }
        
        // Send USB packet (handler on Core 1 will execute it)
        esp_err_t ret = usb_send_packet(&config);
        
        // Update stats
        size_t bytes_tx = config.packet_size;
        if (config.wLength > 0 && (config.bmRequestType & 0x80) == 0) {
            bytes_tx += config.wLength;
        }
        update_usb_stats(bytes_received, bytes_tx);
        
        // Log response
        if (ret == ESP_OK && bytes_received > 0) {
            ESP_LOGI(TAG, "Received %d bytes:", bytes_received);
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, response_buffer, bytes_received, ESP_LOG_INFO);
        } else {
            ESP_LOGW(TAG, "No response received (ret=%s, bytes=%d)", esp_err_to_name(ret), bytes_received);
        }
        
        // Build JSON response with received data
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "status", ret == ESP_OK ? "success" : "failed");
        cJSON_AddNumberToObject(root, "bmRequestType", bmRequestType);
        cJSON_AddNumberToObject(root, "bRequest", bRequest);
        cJSON_AddNumberToObject(root, "wValue", wValue);
        cJSON_AddNumberToObject(root, "wIndex", wIndex);
        cJSON_AddNumberToObject(root, "wLength", wLength);
        cJSON_AddNumberToObject(root, "packet_size", packetSize);
        cJSON_AddNumberToObject(root, "max_retries", maxRetries);
        cJSON_AddNumberToObject(root, "bytes_received", bytes_received);
        cJSON_AddBoolToObject(root, "connected", usb_is_device_connected());
        
        // Add hex dump and ASCII string of response data
        static char hex_str[HTTP_HEX_STR_MAX_LEN];
        static char ascii_str[HTTP_ASCII_STR_MAX_LEN];
        if (bytes_received > 0) {
            size_t hex_len = 0;
            size_t ascii_len = 0;
            size_t max_bytes = bytes_received > HTTP_RESPONSE_MAX_BYTES ? HTTP_RESPONSE_MAX_BYTES : bytes_received;
            
            // Build hex dump
            for (size_t i = 0; i < max_bytes; i++) {
                hex_len += sprintf(hex_str + hex_len, "%02x ", response_buffer[i]);
            }
            if (bytes_received > HTTP_RESPONSE_MAX_BYTES) {
                sprintf(hex_str + hex_len, "... (%d more)", bytes_received - HTTP_RESPONSE_MAX_BYTES);
            }
            
            // Build ASCII string (printable chars only, others shown as '.')
            for (size_t i = 0; i < max_bytes; i++) {
                char c = response_buffer[i];
                ascii_str[ascii_len++] = (c >= 32 && c <= 126) ? c : '.';
            }
            ascii_str[ascii_len] = '\0';
            
            cJSON_AddStringToObject(root, "data", hex_str);
            cJSON_AddStringToObject(root, "ascii", ascii_str);
        } else if (ret == ESP_OK) {
            // 0-byte response but success (e.g. SET_INTERFACE)
            cJSON_AddStringToObject(root, "data", "");
            cJSON_AddStringToObject(root, "ascii", "");
        } else {
            // Actual failure - include error type
            const char *error_str;
            switch (ret) {
                case ESP_ERR_TIMEOUT:      error_str = "TIMEOUT"; break;
                case ESP_ERR_INVALID_RESPONSE: error_str = "NAK"; break;
                case ESP_FAIL:             error_str = "ERROR"; break;
                default:                   error_str = "FAILED"; break;
            }
            cJSON_AddStringToObject(root, "data", error_str);
            cJSON_AddStringToObject(root, "ascii", "");
        }
        
        const char *json_str = cJSON_Print(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json_str);
        
        free((void *)json_str);
        cJSON_Delete(root);
    }
    
    return ESP_OK;
}

// API handler: Send oversized packet
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

// Root handler - serve embedded HTML
static esp_err_t root_handler(httpd_req_t *req)
{
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
    // HTTP server configuration (will run on Core 0 with WiFi)
    // USB handler is now in usb_malformed.c on Core 1
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 32;
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
        
        httpd_uri_t api_request_uri = {
            .uri = "/api/send_request",
            .method = HTTP_POST,
            .handler = api_send_request_handler
        };
        httpd_register_uri_handler(server, &api_request_uri);
        
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

