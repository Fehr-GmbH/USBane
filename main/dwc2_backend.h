/*
 * USBane - DWC2 Hardware USB Backend
 * 
 * Direct DWC2 USB controller access for security research.
 * Uses ESP32-S3's internal USB-OTG hardware.
 * 
 * Copyright (C) 2026 Fehr GmbH
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DWC2_BACKEND_H
#define DWC2_BACKEND_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// USB Constants
#define USB_SETUP_PACKET_SIZE           8
#define USB_CONTROL_EP0_MPS             64
#define USB_MAX_EXTRA_DATA              248     // 256 - 8
#define USB_DMA_CHUNK_SIZE              64
#define USB_DEFAULT_TIMEOUT_MS          1000
#define USB_MAX_NAK_RETRIES             100
#define USB_DEVICE_DESCRIPTOR_SIZE      18
#define USB_MAX_PACKET_SIZE             256

// USB FIFO Sizes
#define USB_RX_FIFO_SIZE                512
#define USB_TX_FIFO_SIZE                512
#define USB_TX_FIFO_START               256
#define USB_PTX_FIFO_SIZE               1024

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// USB Configuration and Data Structures
// ============================================================================

/**
 * @brief USB packet configuration structure
 * All parameters are fully customizable for testing/fuzzing
 */
typedef struct {
    // Standard SETUP packet fields (8 bytes total)
    uint8_t bmRequestType;      // Request type: direction|type|recipient
    uint8_t bRequest;           // Request code (e.g., GET_DESCRIPTOR = 0x06)
    uint16_t wValue;            // Request-specific value
    uint16_t wIndex;            // Request-specific index
    uint16_t wLength;           // Data stage length
    
    // Extended/malformed parameters
    size_t packet_size;         // SETUP packet size (8 = normal, >8 = malformed)
    uint8_t extra_data[248];    // Extra bytes for oversized packets (max 256 total)
    size_t extra_data_len;      // Actual length of extra_data used
    
    // Transfer configuration
    uint8_t device_addr;        // Device address (0 during enumeration)
    uint8_t endpoint;           // Endpoint number (0 for control)
    uint16_t max_packet_size;   // EP0 max packet size (8/16/32/64)
    
    // Response handling
    bool expect_response;       // Should we wait for a response?
    uint32_t timeout_ms;        // Response timeout in milliseconds
    int max_nak_retries;        // Max NAK retries (0 = no retries, -1 = use default)
    uint8_t *response_buffer;   // Buffer for response data (can be NULL)
    size_t response_buffer_size; // Size of response buffer
    size_t *bytes_received;     // Output: bytes actually received
    
    bool setup_only;            // If true, return immediately after SETUP ACK (skip DATA/STATUS)
    uint8_t data_stage_ep;      // If >0, redirect DATA IN stage to this endpoint (e.g., 10 for EP10)
} usb_packet_config_t;

/**
 * @brief USB device info structure
 */
typedef struct {
    bool connected;
    uint16_t vid;
    uint16_t pid;
    uint8_t device_class;
    uint8_t device_subclass;
    uint8_t device_protocol;
    uint8_t max_packet_size;
    char manufacturer[64];
    char product[64];
    char serial[64];
} usb_device_info_t;

/**
 * @brief Initialize USB Handler (creates mutex for protection)
 * This must be called before any other USB operations.
 * 
 * @return ESP_OK on success
 */
esp_err_t usb_handler_start(void);

/**
 * @brief Initialize USB Host hardware (bypassing normal stack)
 * 
 * @return ESP_OK on success
 */
esp_err_t usbane_init(void);

/**
 * @brief Clear cached device info (call after disconnect)
 */
void usb_clear_device_info_cache(void);

/**
 * @brief Send USB Reset to device
 * 
 * @return ESP_OK on success
 */
esp_err_t usb_send_reset(void);

/**
 * @brief Send a USB packet with fully customizable parameters
 * 
 * This is the ONLY send function you need - all parameters are configurable.
 * Can send standard packets, malformed packets, oversized packets, etc.
 * 
 * @param config Pointer to packet configuration
 * @return ESP_OK on success
 */
esp_err_t usb_send_packet(const usb_packet_config_t *config);

/**
 * @brief Write data to device (DATA OUT stage)
 * 
 * @param data Data to send
 * @param length Data length
 * @param timeout_ms Transfer timeout
 * @return ESP_OK if data sent successfully
 */
esp_err_t usb_write_data(const uint8_t *data, size_t length, uint32_t timeout_ms);

/**
 * @brief Read response from device (DATA IN stage)
 * 
 * @param buffer Buffer to store response
 * @param max_len Maximum bytes to read
 * @param timeout_ms Timeout in milliseconds
 * @param bytes_read Output: actual bytes read
 * @param max_nak_retries Maximum NAK retries (-1 for default)
 * @return ESP_OK if data received, ESP_ERR_TIMEOUT if no response
 */
esp_err_t usb_read_response(uint8_t *buffer, size_t max_len, 
                            uint32_t timeout_ms, size_t *bytes_read,
                            int max_nak_retries);

/**
 * @brief Bulk/Interrupt IN transfer from any endpoint
 * 
 * Sends IN token to specified endpoint and receives data.
 * 
 * @param endpoint Endpoint number (1-15, direction bit optional)
 * @param device_addr Device address
 * @param buffer Buffer to store received data
 * @param max_len Maximum bytes to read
 * @param timeout_ms Transfer timeout
 * @param bytes_read Output: actual bytes received
 * @return ESP_OK if data received, ESP_ERR_TIMEOUT if no response
 */
typedef enum {
    USB_EP_TYPE_BULK = 0,
    USB_EP_TYPE_INTERRUPT = 1,
    USB_EP_TYPE_ISOCHRONOUS = 2
} usb_endpoint_type_t;

esp_err_t usb_endpoint_in(uint8_t endpoint, uint8_t device_addr, usb_endpoint_type_t ep_type,
                          uint8_t channel, uint8_t *buffer, size_t max_len,
                          uint32_t timeout_ms, size_t *bytes_read);

/**
 * @brief Continuous Bulk/Interrupt IN transfer with retry
 * 
 * @param endpoint Endpoint number (1-15)
 * @param device_addr Device address
 * @param ep_type Endpoint type (bulk or interrupt)
 * @param channel USB host channel to use
 * @param buffer Buffer to store received data
 * @param max_len Maximum bytes to read
 * @param max_attempts Maximum retry attempts (use UINT32_MAX for infinite)
 * @param attempt_timeout_ms Timeout per attempt in milliseconds
 * @param bytes_read Output: actual bytes received
 * @return ESP_OK if data received, ESP_ERR_TIMEOUT if all attempts failed
 */
esp_err_t usb_endpoint_in_continuous(uint8_t endpoint, uint8_t device_addr, usb_endpoint_type_t ep_type,
                                    uint8_t channel, uint8_t *buffer, size_t max_len,
                                    uint32_t max_attempts, uint32_t attempt_timeout_ms, size_t *bytes_read);

/**
 * @brief Bulk/Interrupt OUT transfer to any endpoint
 * 
 * Sends data to specified endpoint.
 * 
 * @param endpoint Endpoint number (1-15)
 * @param device_addr Device address
 * @param data Data to send
 * @param length Data length
 * @param timeout_ms Transfer timeout
 * @return ESP_OK if data sent successfully
 */
esp_err_t usb_endpoint_out(uint8_t endpoint, uint8_t device_addr, usb_endpoint_type_t ep_type,
                           uint8_t channel, const uint8_t *data, size_t length,
                           uint32_t timeout_ms);

// Auto-recovery control (useful for exploit chains where resets break state)
void usb_set_auto_recovery_enabled(bool enabled);
bool usb_is_auto_recovery_enabled(void);

/**
 * @brief Check if USB device is still connected
 * 
 * @return true if device is connected
 */
bool usb_is_device_connected(void);

/**
 * @brief Get connection status (for web interface)
 * 
 * @return ESP_OK if connected, ESP_FAIL otherwise
 */
esp_err_t usbane_get_conn_status(void);

/**
 * @brief Get connected USB device information
 * 
 * @param info Pointer to device info structure to fill
 * @return ESP_OK if device connected and info retrieved, ESP_FAIL otherwise
 */
esp_err_t usb_get_device_info(usb_device_info_t *info);

/**
 * @brief Save USB PHY configuration to NVS (requires reboot to apply)
 * 
 * @param otg_mode USB OTG mode (0=Host, 1=Device)
 * @param usb_backend USB backend (0=DWC2 Hardware, 1=GPIO Soft-Host)
 * @return ESP_OK on success
 */
esp_err_t usb_save_phy_config(uint8_t otg_mode, uint8_t usb_backend);

// ============================================================================
// Helper Function
// ============================================================================

/**
 * @brief Create default packet config (GET_DESCRIPTOR for device descriptor)
 */
usb_packet_config_t usb_packet_config_default(void);

// ============================================================================
// Direct Implementation Functions (for use by usb_backend.c on Core 1)
// These bypass the internal Core 1 wrapper - caller must ensure Core 1 context
// ============================================================================

esp_err_t dwc2_init_impl(void);
esp_err_t dwc2_send_reset_impl(void);
esp_err_t dwc2_send_packet_impl(const usb_packet_config_t *config);
esp_err_t dwc2_get_device_info_impl(usb_device_info_t *info);

typedef struct {
    uint8_t endpoint;
    uint8_t device_addr;
    usb_endpoint_type_t ep_type;
    uint8_t channel;
    uint8_t *buffer;           // For IN: receive buffer; For OUT: can be cast from data
    const uint8_t *data;       // For OUT: data to send
    size_t max_len;            // Buffer/data length
    uint32_t timeout_ms;
    size_t *bytes_read;        // For IN: output bytes received
    int8_t data_pid;           // Data PID: 0=DATA0, 2=DATA1, -1=auto(DATA0)
} usb_endpoint_params_t;

esp_err_t dwc2_endpoint_in_impl(const usb_endpoint_params_t *params);
esp_err_t dwc2_endpoint_out_impl(const usb_endpoint_params_t *params);
esp_err_t dwc2_vbus_power_cycle_impl(void);

// Check if device connected (no Core 1 wrapper needed)
bool dwc2_is_device_connected(void);

#ifdef __cplusplus
}
#endif

#endif // DWC2_BACKEND_H
