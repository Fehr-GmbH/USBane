/**
 * USBane Native Chain Engine
 * 
 * Executes USB request chains entirely in C for microsecond timing.
 * CSV is parsed in memory and executed directly.
 * Results are streamed back via WebSocket.
 */

#ifndef CHAIN_ENGINE_H
#define CHAIN_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Maximum chain size
#define CHAIN_MAX_ENTRIES 256
#define CHAIN_MAX_DATA_LEN 64

// Chain entry types
typedef enum {
    CHAIN_TYPE_CONTROL = 0,      // USB Control Transfer (EP0)
    CHAIN_TYPE_BULK_IN,          // Bulk IN (EP1-15)
    CHAIN_TYPE_BULK_OUT,         // Bulk OUT (EP1-15)
    CHAIN_TYPE_INTERRUPT_IN,     // Interrupt IN (EP1-15)
    CHAIN_TYPE_INTERRUPT_OUT,    // Interrupt OUT (EP1-15)
    CHAIN_TYPE_ISO_IN,           // Isochronous IN (EP1-15)
    CHAIN_TYPE_ISO_OUT,          // Isochronous OUT (EP1-15)
    CHAIN_TYPE_WAIT_DELAY,       // Wait: fixed delay
    CHAIN_TYPE_WAIT_GPIO,        // Wait: GPIO pin state
    CHAIN_TYPE_WAIT_USB_RESET,   // Wait: USB reset
    CHAIN_TYPE_WAIT_VBUS_CYCLE,  // Wait: VBUS power cycle (full device reset)
    CHAIN_TYPE_WAIT_BUTTON,      // Wait: user button click (via WebSocket)
    CHAIN_TYPE_WAIT_WEBHOOK,     // Wait: external webhook trigger
    CHAIN_TYPE_ACTION_COPY,      // Action: copy data between requests
    CHAIN_TYPE_ACTION_GOTO,      // Action: jump to index
    CHAIN_TYPE_ACTION_COMMENT,   // Action: comment (no-op, for logging)
    CHAIN_TYPE_ACTION_GPIO_OUT,  // Action: set GPIO output level
    CHAIN_TYPE_ACTION_HTTP,      // Action: HTTP request
    CHAIN_TYPE_ACTION_CONFIG,    // Action: config toggle (auto-recovery, etc.)
    CHAIN_TYPE_ACTION_ADD32,     // Action: add to 32-bit value across 4 entries' field (wValue/wIndex/wLength)
    CHAIN_TYPE_CONDITION,        // Conditional: compare and branch
} chain_entry_type_t;

// Condition operators
typedef enum {
    CHAIN_OP_EQ = 0,    // ==
    CHAIN_OP_NE,        // !=
    CHAIN_OP_LT,        // <
    CHAIN_OP_GT,        // >
    CHAIN_OP_LE,        // <=
    CHAIN_OP_GE,        // >=
    CHAIN_OP_CONTAINS,  // contains
} chain_operator_t;

// Condition actions
typedef enum {
    CHAIN_COND_CONTINUE = 0,  // Continue execution
    CHAIN_COND_SKIP,          // Skip next entry
    CHAIN_COND_BREAK,         // Stop chain
} chain_cond_action_t;

// Flags for control transfers
#define CHAIN_FLAG_NO_RETRY     (1 << 0)
#define CHAIN_FLAG_SETUP_ONLY   (1 << 1)

// Compact binary entry (32 bytes each for alignment)
typedef struct __attribute__((packed)) {
    uint8_t type;           // chain_entry_type_t
    uint8_t flags;          // CHAIN_FLAG_*
    
    union {
        // Control transfer (type == CHAIN_TYPE_CONTROL)
        struct {
            uint8_t bmRequestType;
            uint8_t bRequest;
            uint16_t wValue;
            uint16_t wIndex;
            uint16_t wLength;
            uint8_t packetSize;
            uint8_t deviceAddr;
            uint8_t dataStageEp;
            uint8_t dataLen;
            uint8_t data[16];   // Inline data for small payloads
        } ctrl;
        
        // Endpoint transfer (bulk/interrupt IN/OUT)
        struct {
            uint8_t endpoint;
            uint8_t deviceAddr;
            uint16_t length;
            uint16_t timeout;
            uint8_t continuous;
            uint8_t maxAttempts;
            uint8_t dataLen;
            uint8_t data[64];   // For OUT transfers (increased for SCSI/etc)
        } ep;
        
        // Wait delay
        struct {
            uint32_t duration_ms;
        } delay;
        
        // Wait GPIO
        struct {
            uint8_t pin;
            uint8_t level;
            uint16_t timeout_sec;
        } gpio;
        
        // Copy action
        struct {
            int8_t fromReqNo;     // -1 = last
            uint8_t fromOffset;
            uint8_t fromLength;
            int8_t toReqNo;       // -1 = next
            uint8_t toField;      // 0=wValue, 1=wIndex, 2=wLength, 3=data, etc.
        } copy;
        
        // Goto action
        struct {
            uint16_t targetIndex;
        } jump;
        
        // GPIO output action
        struct {
            uint8_t pin;
            uint8_t level;
        } gpio_out;
        
        // Button wait
        struct {
            uint16_t timeout_sec;     // 0 = infinite
            char label[24];           // Label to show in modal
        } button;
        
        // Webhook wait
        struct {
            uint16_t timeout_sec;
            char trigger_id[24];      // Trigger ID for webhook
        } webhook;
        
        // HTTP action
        struct {
            char url[64];             // URL to request
            uint8_t method;           // 0=GET, 1=POST
        } http;

        // Config action
        struct {
            char key[16];
            char value[16];
        } config;

        // Add32 action: add increment to 32-bit value across 4 entries
        struct {
            uint32_t increment;
            uint8_t entryIdx[4];  // entry indices for bytes 0(LSB)-3(MSB)
            uint8_t field;        // 0=wValue (default), 1=wIndex, 2=wLength
        } add32;
        
        // Condition
        struct {
            uint8_t operator;     // chain_operator_t
            uint8_t action;       // chain_cond_action_t
            int8_t aReqNo;
            uint8_t aOffset;
            uint8_t aLength;
            int8_t bReqNo;
            uint8_t bOffset;
            uint8_t bLength;
            uint8_t bManualValue[8];  // For manual comparison value
        } cond;
        
        // Padding to ensure fixed size
        uint8_t _pad[30];
    };
} chain_entry_t;

// Execution result for single entry
typedef struct {
    uint8_t type;
    uint8_t status;         // 0=success, 1=timeout, 2=error, 3=skipped
    uint16_t bytes_received;
    uint8_t data[256];      // Response data
    uint16_t data_len;
    
    // USB control params (for bruteforce results)
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
    uint8_t packetSize;
} chain_result_t;

// Callback for streaming results (called after each entry)
typedef void (*chain_result_callback_t)(int index, const chain_result_t *result, void *user_data);

// Callback for waiting state notifications (called when chain starts waiting for button/webhook)
// type: CHAIN_TYPE_WAIT_BUTTON or CHAIN_TYPE_WAIT_WEBHOOK
// info: label for button, trigger_id for webhook
typedef void (*chain_wait_callback_t)(int index, chain_entry_type_t type, const char *info, void *user_data);

// Callback for job completion
typedef void (*chain_done_callback_t)(esp_err_t result, void *user_data);

// ============================================================================
// USB Executor Job Types (all run on Core 1)
// ============================================================================

typedef enum {
    USB_JOB_CHAIN,          // CSV chain execution (also used for single requests)
    USB_JOB_BRUTEFORCE,     // Bruteforce iteration
} usb_job_type_t;

// Transfer type for single requests
typedef enum {
    USB_TRANSFER_CONTROL,       // Control transfer (EP0)
    USB_TRANSFER_BULK_IN,       // Bulk IN transfer
    USB_TRANSFER_BULK_OUT,      // Bulk OUT transfer
    USB_TRANSFER_INTERRUPT_IN,  // Interrupt IN transfer
    USB_TRANSFER_INTERRUPT_OUT, // Interrupt OUT transfer
} usb_transfer_type_t;

// Single request job parameters (supports all transfer types)
typedef struct {
    usb_transfer_type_t type;   // Transfer type
    uint8_t deviceAddr;         // USB device address
    uint32_t timeout;           // Timeout in ms
    uint8_t data[256];          // Data buffer (IN: receives, OUT: sends)
    uint16_t dataLen;           // Data length (IN: max, OUT: actual)
    
    // Control transfer specific
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
    uint8_t packetSize;
    uint8_t dataStageEp;
    bool setupOnly;
    
    // Endpoint transfer specific
    uint8_t endpoint;           // Endpoint number (1-15)
    uint8_t channel;            // USB host channel
    bool continuous;            // Continuous polling mode
    uint32_t maxAttempts;       // Max attempts for continuous mode
} usb_job_single_t;

// Bruteforce field configuration
typedef struct {
    uint16_t start;
    uint16_t end;
    bool iterate;
} usb_bf_field_t;

// Bruteforce job parameters
typedef struct {
    usb_bf_field_t bmRequestType;
    usb_bf_field_t bRequest;
    usb_bf_field_t wValueHi;
    usb_bf_field_t wValueLo;
    usb_bf_field_t wIndexHi;
    usb_bf_field_t wIndexLo;
    usb_bf_field_t wLength;
    usb_bf_field_t packetSize;
    uint32_t delay_ms;
} usb_job_bruteforce_t;

// Chain job parameters
typedef struct {
    char *csv_data;         // Allocated by caller, freed by executor
    size_t csv_len;
} usb_job_chain_t;

// ============================================================================
// Chain API
// ============================================================================

/**
 * Initialize chain engine (call once at startup)
 */
esp_err_t chain_engine_init(void);

/**
 * Execute chain from CSV data
 * 
 * Parses CSV in memory and executes directly (no NVS storage).
 * Comments (lines starting with #) are stripped.
 * 
 * @param csv_data CSV string data
 * @param csv_len Length of CSV data
 * @param result_cb Called after each entry with result (can be NULL)
 * @param wait_cb Called when chain starts waiting for button/webhook (can be NULL)
 * @param user_data Passed to callbacks
 * @return ESP_OK if chain completed successfully
 */
esp_err_t chain_execute_csv(const char *csv_data, size_t csv_len, 
                            chain_result_callback_t result_cb,
                            chain_wait_callback_t wait_cb,
                            void *user_data);

/**
 * Stop currently executing chain (can be called from another task)
 */
void chain_stop(void);

/**
 * Check if chain is currently executing
 */
bool chain_is_running(void);

/**
 * Check if chain is waiting for button press
 */
bool chain_is_waiting_button(void);

/**
 * Get the current button wait label (if waiting)
 */
const char* chain_get_button_label(void);

/**
 * Signal button press to resume chain execution
 */
void chain_button_continue(void);

/**
 * Check if chain is waiting for webhook
 */
bool chain_is_waiting_webhook(void);

/**
 * Get the current webhook trigger ID (if waiting)
 */
const char* chain_get_webhook_trigger_id(void);

/**
 * Signal webhook trigger to resume chain execution
 */
void chain_webhook_trigger(const char *trigger_id);

// ============================================================================
// USB Executor API (runs jobs on Core 1)
// ============================================================================

/**
 * Submit a chain execution job to Core 1
 * 
 * @param csv_data CSV data (will be copied internally)
 * @param csv_len Length of CSV data
 * @param result_cb Called after each entry
 * @param wait_cb Called when waiting for button/webhook
 * @param done_cb Called when job completes
 * @param user_data Passed to all callbacks
 * @return ESP_OK if job submitted, ESP_ERR_INVALID_STATE if busy
 */
esp_err_t usb_executor_submit_chain(const char *csv_data, size_t csv_len,
                                    chain_result_callback_t result_cb,
                                    chain_wait_callback_t wait_cb,
                                    chain_done_callback_t done_cb,
                                    void *user_data);

/**
 * Submit a bruteforce job to Core 1
 */
esp_err_t usb_executor_submit_bruteforce(const usb_job_bruteforce_t *params,
                                         chain_result_callback_t result_cb,
                                         chain_done_callback_t done_cb,
                                         void *user_data);

/**
 * Check if executor is busy
 */
bool usb_executor_is_busy(void);

/**
 * Stop current job
 */
void usb_executor_stop(void);

#endif // CHAIN_ENGINE_H
