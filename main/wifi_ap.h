/*
 * USBane - WiFi Module
 */

#ifndef WIFI_AP_H
#define WIFI_AP_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize WiFi (AP or STA mode based on NVS config)
 */
void wifi_ap_init(void);

/**
 * @brief Check if connected to external WiFi (STA mode)
 * @return true if connected, false otherwise
 */
bool wifi_is_sta_connected(void);

/**
 * @brief Get the IP address when in STA mode
 * @param ip_str Buffer to store IP string
 * @param len Length of buffer
 * @return ESP_OK on success
 */
esp_err_t wifi_get_sta_ip(char *ip_str, size_t len);

#ifdef __cplusplus
}
#endif

#endif // WIFI_AP_H

