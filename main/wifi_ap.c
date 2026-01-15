/*
 * USBane - WiFi Module
 * Supports both AP mode (host own network) and STA mode (connect to router)
 */

#include "wifi_ap.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/ip_addr.h"
#include <string.h>

static const char *TAG = "WIFI";

// Default AP settings
#define DEFAULT_AP_SSID "USBane"
#define DEFAULT_AP_PASS "usbane123"
#define AP_CHANNEL 1
#define AP_MAX_CONN 4

// WiFi event group
static esp_netif_t *ap_netif = NULL;
static esp_netif_t *sta_netif = NULL;
static bool sta_connected = false;
static int sta_retry_count = 0;
#define STA_MAX_RETRY 10

// Event handler for WiFi events
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "STA started, connecting...");
                sta_retry_count = 0;
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                sta_connected = false;
                if (sta_retry_count < STA_MAX_RETRY) {
                    sta_retry_count++;
                    ESP_LOGW(TAG, "WiFi disconnected, retry %d/%d...", sta_retry_count, STA_MAX_RETRY);
                    esp_wifi_connect();
                } else {
                    ESP_LOGE(TAG, "WiFi connection failed after %d retries", STA_MAX_RETRY);
                    ESP_LOGI(TAG, "Fallback AP still active at 192.168.4.1");
                }
                break;
            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t* evt = (wifi_event_ap_staconnected_t*) event_data;
                ESP_LOGI(TAG, "Station joined, AID=%d", evt->aid);
                break;
            }
            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t* evt = (wifi_event_ap_stadisconnected_t*) event_data;
                ESP_LOGI(TAG, "Station left, AID=%d", evt->aid);
                break;
            }
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
            ESP_LOGI(TAG, "Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
            sta_connected = true;
            sta_retry_count = 0;
        }
    }
}

static void init_ap_mode(const char *ssid, const char *password)
{
    ap_netif = esp_netif_create_default_wifi_ap();
    
    // Configure static IP
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(ap_netif);
    esp_netif_set_ip_info(ap_netif, &ip_info);
    esp_netif_dhcps_start(ap_netif);
    
    wifi_config_t wifi_config = {
        .ap = {
            .channel = AP_CHANNEL,
            .max_connection = AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
        },
    };
    
    strncpy((char *)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid_len = strlen(ssid);
    strncpy((char *)wifi_config.ap.password, password, sizeof(wifi_config.ap.password) - 1);
    
    // Allow open network if no password
    if (strlen(password) < 8) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "AP Mode started");
    ESP_LOGI(TAG, "SSID: %s", ssid);
    ESP_LOGI(TAG, "IP: 192.168.4.1");
}

static void init_sta_mode(const char *ssid, const char *password)
{
    // Create both AP and STA - we keep AP running as fallback
    ap_netif = esp_netif_create_default_wifi_ap();
    sta_netif = esp_netif_create_default_wifi_sta();
    
    // Configure AP static IP
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(ap_netif);
    esp_netif_set_ip_info(ap_netif, &ip_info);
    esp_netif_dhcps_start(ap_netif);
    
    // Load AP settings for fallback
    char ap_ssid[33] = DEFAULT_AP_SSID;
    char ap_pass[65] = DEFAULT_AP_PASS;
    
    nvs_handle_t nvs;
    if (nvs_open("wifi_config", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(ap_ssid);
        nvs_get_str(nvs, "ap_ssid", ap_ssid, &len);
        len = sizeof(ap_pass);
        nvs_get_str(nvs, "ap_pass", ap_pass, &len);
        nvs_close(nvs);
    }
    
    // Configure AP (fallback)
    wifi_config_t ap_config = {
        .ap = {
            .channel = AP_CHANNEL,
            .max_connection = AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = { .required = false },
        },
    };
    strncpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen(ap_ssid);
    strncpy((char *)ap_config.ap.password, ap_pass, sizeof(ap_config.ap.password) - 1);
    if (strlen(ap_pass) < 8) ap_config.ap.authmode = WIFI_AUTH_OPEN;
    
    // Configure STA
    wifi_config_t sta_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password) - 1);
    
    // Use APSTA mode so we can always access the device
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "APSTA Mode started");
    ESP_LOGI(TAG, "Connecting to: %s", ssid);
    ESP_LOGI(TAG, "Fallback AP: %s (192.168.4.1)", ap_ssid);
}

void wifi_ap_init(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Initialize network interface
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));
    
    // WiFi init
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // Read mode from NVS
    char mode[8] = "ap";
    char sta_ssid[33] = {0};
    char sta_pass[65] = {0};
    char ap_ssid[33] = DEFAULT_AP_SSID;
    char ap_pass[65] = DEFAULT_AP_PASS;
    
    nvs_handle_t nvs;
    if (nvs_open("wifi_config", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len;
        
        len = sizeof(mode);
        nvs_get_str(nvs, "mode", mode, &len);
        
        len = sizeof(sta_ssid);
        nvs_get_str(nvs, "sta_ssid", sta_ssid, &len);
        
        len = sizeof(sta_pass);
        nvs_get_str(nvs, "sta_pass", sta_pass, &len);
        
        len = sizeof(ap_ssid);
        nvs_get_str(nvs, "ap_ssid", ap_ssid, &len);
        
        len = sizeof(ap_pass);
        nvs_get_str(nvs, "ap_pass", ap_pass, &len);
        
        nvs_close(nvs);
        
        ESP_LOGI(TAG, "Loaded WiFi config: mode=%s", mode);
    }
    
    // Initialize based on mode
    if (strcmp(mode, "sta") == 0 && strlen(sta_ssid) > 0) {
        init_sta_mode(sta_ssid, sta_pass);
    } else {
        init_ap_mode(ap_ssid, ap_pass);
    }
}

bool wifi_is_sta_connected(void)
{
    return sta_connected;
}

esp_err_t wifi_get_sta_ip(char *ip_str, size_t len)
{
    if (!sta_netif || !sta_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_netif_ip_info_t ip_info;
    esp_err_t ret = esp_netif_get_ip_info(sta_netif, &ip_info);
    if (ret == ESP_OK) {
        snprintf(ip_str, len, IPSTR, IP2STR(&ip_info.ip));
    }
    return ret;
}
