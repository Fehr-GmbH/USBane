/*
 * USBane - Web Interface
 */

#ifndef WEB_INTERFACE_H
#define WEB_INTERFACE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the web server for attack control
 * 
 * @return ESP_OK on success
 */
esp_err_t web_interface_start(void);

/**
 * @brief Stop the web server
 * 
 * @return ESP_OK on success
 */
esp_err_t web_interface_stop(void);

#ifdef __cplusplus
}
#endif

#endif // WEB_INTERFACE_H

