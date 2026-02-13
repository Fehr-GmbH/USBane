/*
 * USBane - GPIO Soft-Host USB Backend (Full-Speed 12 Mbps)
 * 
 * Full-Speed USB host via GPIO bit-banging on ESP32-S3 @ 240MHz.
 * 
 * DERIVED WORK: Based on esp32_usb_soft_host by Dima Samsonov
 *   Original author: Dima Samsonov @ Israel (sdima1357@gmail.com) - 3/2021
 *   Repository: https://github.com/sdima1357/esp32_usb_soft_host
 * 
 * Copyright (C) 2021 Dmitry Samsonov - Original Low-Speed implementation
 * Copyright (C) 2026 Fehr GmbH - Full-Speed port and USBane integration
 * 
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef SOFT_BACKEND_H
#define SOFT_BACKEND_H

#include <stdint.h>
#include <stddef.h>

// GCC-compatible packed attribute
#ifndef __packed
#define __packed __attribute__((packed))
#endif

#define TIMER_DIVIDER         2
#define TIMER_SCALE           (TIMER_BASE_CLK / TIMER_DIVIDER)
#define TIMER_INTERVAL0_SEC   (0.001)

// USB address constants
#define ZERO_USB_ADDRESS      0
#define ASSIGNED_USB_ADDRESS  3

// Number of USB ports supported
#define NUM_USB 1  // Only support 1 USB port for simplicity

// ============================================================================
// Initialization
// ============================================================================

/**
 * @brief Initialize soft-host GPIO pins
 * 
 * @param DP0 D+ pin for port 0 (-1 to disable)
 * @param DM0 D- pin for port 0 (-1 to disable)
 * @param DP1-DM3 Pins for ports 1-3 (-1 to disable)
 */
void initStates(int DP0, int DM0, int DP1, int DM1, int DP2, int DM2, int DP3, int DM3);

/**
 * @brief Main USB processing loop (call periodically)
 * 
 * Handles USB state machine and bit-bang communication.
 * Must be called frequently for USB timing.
 */
void usb_process(void);

/**
 * @brief Print current USB state (debug)
 */
void printState(void);

// ============================================================================
// USB Transfers
// ============================================================================

// Token types for USB packets
#define T_SETUP     0b10110100
#define T_IN        0b10010110
#define T_OUT       0b10000111
#define T_DATA0     0b11000011
#define T_DATA1     0b11010010

/**
 * @brief Send a control request (SETUP + optional IN data stage)
 */
void Request(uint8_t cmd, uint8_t addr, uint8_t eop,
             uint8_t dataCmd, uint8_t bmRequestType, uint8_t bmRequest,
             uint16_t wValue, uint16_t wIndex, uint16_t wLen, uint16_t waitForBytes);

/**
 * @brief Send a control request with data (SETUP + OUT data stage)
 */
void RequestSend(uint8_t cmd, uint8_t addr, uint8_t eop,
                 uint8_t dataCmd, uint8_t bmRequestType, uint8_t bmRequest,
                 uint16_t wValue, uint16_t wIndex, uint16_t wLen,
                 uint16_t transmitL1Bytes, uint8_t* data);

/**
 * @brief Send an oversized SETUP packet (for exploit research)
 * 
 * Appends extra_data bytes after the standard 8-byte SETUP packet.
 * The total packet size will be 8 + extra_len bytes.
 */
void RequestOversized(uint8_t cmd, uint8_t addr, uint8_t eop,
                      uint8_t dataCmd, uint8_t bmRequestType, uint8_t bmRequest,
                      uint16_t wValue, uint16_t wIndex, uint16_t wLen,
                      uint16_t waitForBytes,
                      const uint8_t *extra_data, size_t extra_len);

/**
 * @brief Send an IN request to an endpoint
 */
void RequestIn(uint8_t cmd, uint8_t addr, uint8_t eop, uint16_t waitForBytes);

// ============================================================================
// Transfer Status
// ============================================================================

/**
 * @brief Set current port for operations
 */
void usb_set_current_port(int port);

/**
 * @brief Check if transfer is complete
 * @return 1 if complete, 0 if in progress
 */
int usb_is_transfer_complete(int port);

/**
 * @brief Check if a Low-Speed device is connected on a port
 * @param port Port number (0-3)
 * @return 1 if device detected, 0 if not
 */
int soft_host_is_device_connected(int port);

/**
 * @brief Get response buffer after transfer
 * @param port Port number
 * @param len Output: length of data
 * @return Pointer to response data
 */
uint8_t* usb_get_response_buffer(int port, uint8_t *len);

/**
 * @brief Get debug info for a port (cb_Cmd, fsm_state, ack/nack counts, wire state)
 */
void usb_get_debug_info(int port, int *cb_cmd, int *fsm_state, 
                        int *ack_count, int *nack_count, int *wire_state);

// ============================================================================
// Flags
// ============================================================================

void usbSetFlags(int _usb_num, uint8_t flags);
uint8_t usbGetFlags(int _usb_num);

// ============================================================================
// Callbacks (must be implemented by application)
// ============================================================================

/**
 * @brief LED control callback
 */
void led(int on_off);

/**
 * @brief USB message callback (called when data received)
 */
void usbMess(uint8_t src, uint8_t len, uint8_t *data);

// ============================================================================
// USB Descriptor Structures
// ============================================================================

typedef __packed struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
} sDevDesc;

typedef __packed struct {
    uint8_t bLength;
    uint8_t bType;
    uint16_t wLength;
    uint8_t bNumIntf;
    uint8_t bCV;
    uint8_t bIndex;
    uint8_t bAttr;
    uint8_t bMaxPower;
} sCfgDesc;

typedef __packed struct {
    uint8_t bLength;
    uint8_t bType;
    uint8_t iNum;
    uint8_t iAltString;
    uint8_t bEndPoints;
    uint8_t iClass;
    uint8_t iSub;
    uint8_t iProto;
    uint8_t iIndex;
} sIntfDesc;

typedef __packed struct {
    uint8_t bLength;
    uint8_t bType;
    uint8_t bEPAdd;
    uint8_t bAttr;
    uint16_t wPayLoad;
    uint8_t bInterval;
} sEPDesc;

typedef __packed struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdHID;
    uint8_t bCountryCode;
    uint8_t bNumDescriptors;
    uint8_t bReportDescriptorType;
    uint8_t wItemLengthL;
    uint8_t wItemLengthH;
} HIDDescriptor;

typedef __packed struct {
    uint8_t bLength;
    uint8_t bType;
    uint16_t wLang;
} sStrDesc;

#endif // SOFT_BACKEND_H
