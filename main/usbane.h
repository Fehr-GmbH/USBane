/*
 * USBane - USB Backend Router
 * 
 * Unified interface for USB operations that routes to:
 * - DWC2 hardware USB host (Full-Speed/Low-Speed via USB-OTG port)
 * - GPIO soft-host (Full-Speed via GPIO bit-banging) [WIP - see LIMITATIONS.md]
 *
 * ALL USB operations run on Core 1 via the worker task.
 *
 * Copyright 2026 Fehr GmbH
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef USBANE_H
#define USBANE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "dwc2_backend.h"  // For usb_packet_config_t, usb_endpoint_type_t

/**
 * @brief USB backend type selection
 */
typedef enum {
    USB_BACKEND_DWC2,       /**< Hardware USB-OTG via DWC2 controller */
    USB_BACKEND_SOFT_HOST   /**< GPIO bit-bang soft-host (Low-Speed only) */
} usb_backend_type_t;

/**
 * @brief Soft-host GPIO pin configuration
 */
typedef struct {
    int dp_pin;             /**< D+ GPIO pin number */
    int dm_pin;             /**< D- GPIO pin number */
} usb_soft_host_pins_t;

// ============================================================================
// Initialization (called from main.c)
// ============================================================================

/**
 * @brief Start USB backend worker task on Core 1
 * 
 * This starts the worker that handles ALL USB operations.
 * Call this once at boot before usb_backend_init().
 * 
 * @return ESP_OK on success
 */
esp_err_t usb_backend_start_worker(void);

/**
 * @brief Initialize USB backend based on NVS config
 * 
 * Reads backend type from NVS and initializes either:
 * - DWC2 hardware controller, or
 * - GPIO soft-host pins
 * 
 * Must be called after usb_backend_start_worker().
 * Executes on Core 1 worker.
 * 
 * @return ESP_OK on success
 */
esp_err_t usb_backend_init(void);

/**
 * @brief Deinitialize the USB backend
 * @return ESP_OK on success
 */
esp_err_t usb_backend_deinit(void);

// ============================================================================
// Backend Info
// ============================================================================

/**
 * @brief Get the currently active backend type
 * @return Current backend type
 */
usb_backend_type_t usb_backend_get_type(void);

/**
 * @brief Check if using soft-host backend (fast cached check)
 * @return true if soft-host is active
 */
bool usb_backend_is_soft_host(void);

/**
 * @brief Get backend name string
 * @param type Backend type
 * @return Human-readable backend name
 */
const char *usb_backend_type_name(usb_backend_type_t type);

/**
 * @brief Check if soft-host backend is available
 * @return true (always available on ESP32-S3)
 */
bool usb_backend_soft_host_available(void);

// ============================================================================
// Configuration (call before usb_backend_init)
// ============================================================================

/**
 * @brief Set the active backend type (before init)
 * @param type Backend type to use
 * @return ESP_OK on success
 */
esp_err_t usb_backend_set_type(usb_backend_type_t type);

/**
 * @brief Configure soft-host GPIO pins (before init)
 * @param pins GPIO pin configuration
 * @return ESP_OK on success
 */
esp_err_t usb_backend_set_soft_host_pins(const usb_soft_host_pins_t *pins);

/**
 * @brief Get soft-host GPIO pin configuration
 * @param pins Output: current pin configuration
 * @return ESP_OK on success
 */
esp_err_t usb_backend_get_soft_host_pins(usb_soft_host_pins_t *pins);

// ============================================================================
// USB Operations (all execute on Core 1)
// ============================================================================

/**
 * @brief Perform a USB bus reset
 * @return ESP_OK on success
 */
esp_err_t usb_backend_reset(void);

/**
 * @brief Execute a control transfer
 * 
 * Routes to DWC2 or soft-host based on active backend.
 * Executes on Core 1 worker task.
 * 
 * @param config Full packet configuration (DWC2-style)
 * @return ESP_OK on success
 */
esp_err_t usb_backend_send_packet(const usb_packet_config_t *config);

/**
 * @brief Execute an endpoint IN transfer
 * 
 * Routes to DWC2 or soft-host based on active backend.
 * Executes on Core 1 worker task.
 */
esp_err_t usb_backend_endpoint_in(
    uint8_t endpoint,
    uint8_t device_addr,
    usb_endpoint_type_t ep_type,
    uint8_t data_toggle,
    uint8_t *data,
    size_t max_len,
    uint32_t timeout_ms,
    size_t *bytes_received);

esp_err_t usb_backend_endpoint_in_with_pid(
    uint8_t endpoint,
    uint8_t device_addr,
    usb_endpoint_type_t ep_type,
    uint8_t channel,
    int8_t data_pid,
    uint8_t *data,
    size_t max_len,
    uint32_t timeout_ms,
    size_t *bytes_received);

/**
 * @brief Execute an endpoint OUT transfer
 * 
 * Routes to DWC2 or soft-host based on active backend.
 * Executes on Core 1 worker task.
 */
esp_err_t usb_backend_endpoint_out(
    uint8_t endpoint,
    uint8_t device_addr,
    usb_endpoint_type_t ep_type,
    uint8_t data_toggle,
    const uint8_t *data,
    size_t len,
    uint32_t timeout_ms);

/**
 * @brief Check if a USB device is connected
 * @return true if device detected
 */
bool usb_backend_is_device_connected(void);

#endif // USBANE_H
