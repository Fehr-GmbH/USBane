/*
 * USBane - ESP32-S3 USB Security Research Tool
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "usb_malformed.h"
#include "wifi_ap.h"
#include "web_interface.h"

static const char *TAG = "USBANE";

void app_main(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "  _   _ ____  ____                   ");
    ESP_LOGI(TAG, " | | | / ___|| __ )  __ _ _ __   ___ ");
    ESP_LOGI(TAG, " | | | \\___ \\|  _ \\ / _` | '_ \\ / _ \\");
    ESP_LOGI(TAG, " | |_| |___) | |_) | (_| | | | |  __/");
    ESP_LOGI(TAG, "  \\___/|____/|____/ \\__,_|_| |_|\\___|");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "  USB Security Research Tool v%s", CONFIG_USBANE_VERSION);
    
    // Initialize WiFi AP
    ESP_LOGI(TAG, "Starting WiFi...");
    wifi_ap_init();
    
    // Start web interface
    ESP_LOGI(TAG, "Starting Web Interface...");
    web_interface_start();
    
    // Wait for network to settle
    ESP_LOGI(TAG, "Waiting 2 seconds...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Start USB Handler Task on Core 1
    ESP_LOGI(TAG, "Starting USB Handler on Core 1...");
    esp_err_t ret = usb_handler_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start USB handler: %s", esp_err_to_name(ret));
        return;
    }
    
    vTaskDelay(pdMS_TO_TICKS(100));  // Let handler start
    
    // Initialize USB Host hardware (via handler on Core 1)
    ESP_LOGI(TAG, "Initializing USB Host hardware...");
    ret = usb_malformed_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "USB init returned: %s (continuing anyway)", esp_err_to_name(ret));
        // Don't return - keep running so web interface works and we can detect device later
    }
    
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "═══════════════════════════════════════════");
    ESP_LOGI(TAG, "USBane READY!");
    ESP_LOGI(TAG, "═══════════════════════════════════════════");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "CONNECT TO WEB INTERFACE:");
    ESP_LOGI(TAG, "  1. WiFi: USBane");
    ESP_LOGI(TAG, "  2. Password: usbane123");
    ESP_LOGI(TAG, "  3. Open: http://192.168.4.1");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Monitoring USB connection status...");
    ESP_LOGI(TAG, "");
    
    // Monitor device connection status
    bool last_connected = false;
    
    while (1) {
        bool connected = usb_is_device_connected();
        
        if (connected != last_connected) {
            if (connected) {
                ESP_LOGI(TAG, "USB Device connected!");
                // Set flag so web interface will reset on next request
                extern bool usb_needs_reset;
                usb_needs_reset = true;
            } else {
                ESP_LOGW(TAG, "USB Device disconnected!");
            }
            last_connected = connected;
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
