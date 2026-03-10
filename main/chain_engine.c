/**
 * USBane Native Chain Engine Implementation
 * 
 * Executes USB request chains entirely in C for microsecond timing.
 * Parses CSV in memory and executes directly (no NVS storage needed).
 */

#include "chain_engine.h"
#include "dwc2_backend.h"
#include "usbane.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <inttypes.h>

static const char *TAG = "CHAIN";

// Execution state
static volatile bool chain_running = false;
static volatile bool chain_stop_requested = false;

// Button wait state
static volatile bool waiting_for_button = false;
static volatile bool button_pressed = false;
static char button_label[32] = {0};
static SemaphoreHandle_t button_semaphore = NULL;

// Webhook wait state
static volatile bool waiting_for_webhook = false;
static volatile bool webhook_triggered = false;
static char webhook_pending_id[32] = {0};
static SemaphoreHandle_t webhook_semaphore = NULL;

// Response storage for copy actions (last N responses)
#define MAX_STORED_RESPONSES 32
static chain_result_t stored_responses[MAX_STORED_RESPONSES];
static int response_count = 0;

// Per-endpoint DATA PID toggle tracker (0=DATA0, 2=DATA1)
static int8_t ep_data_pid[16] = {0};

// ============================================================================
// USB Executor (runs on Core 1)
// ============================================================================

// Job structure
typedef struct {
    usb_job_type_t type;
    chain_result_callback_t result_cb;
    chain_wait_callback_t wait_cb;
    chain_done_callback_t done_cb;
    void *user_data;
    
    union {
        usb_job_chain_t chain;
        usb_job_single_t single;
        usb_job_bruteforce_t bruteforce;
    };
} usb_job_t;

static TaskHandle_t executor_task_handle = NULL;
static SemaphoreHandle_t executor_job_ready = NULL;
static volatile bool executor_busy = false;
static volatile bool executor_stop_requested = false;
static usb_job_t current_job = {0};

// Forward declarations
static void usb_executor_task(void *arg);
static void execute_bruteforce_job(const usb_job_bruteforce_t *params, chain_result_callback_t cb, chain_done_callback_t done_cb, void *user_data);

// ============================================================================
// Helper Functions
// ============================================================================

static uint32_t parse_hex_or_dec(const char *str) {
    if (!str || !*str) return 0;
    while (isspace((unsigned char)*str)) str++;
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        return strtoul(str, NULL, 16);
    }
    return strtoul(str, NULL, 10);
}

static size_t parse_hex_bytes(const char *hex, uint8_t *out, size_t max_len) {
    if (!hex || !*hex) return 0;
    size_t len = 0;
    while (*hex && len < max_len) {
        while (isspace((unsigned char)*hex) || *hex == ',') hex++;
        if (!*hex) break;
        char byte_str[3] = {0};
        byte_str[0] = *hex++;
        if (*hex && isxdigit((unsigned char)*hex)) {
            byte_str[1] = *hex++;
        }
        out[len++] = (uint8_t)strtoul(byte_str, NULL, 16);
    }
    return len;
}

// Split CSV line into fields
static int split_csv_line(char *line, char **fields, int max_fields) {
    int count = 0;
    char *p = line;
    
    while (*p && count < max_fields) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        
        if (*p == '"') {
            p++;
            fields[count++] = p;
            while (*p && *p != '"') p++;
            if (*p == '"') *p++ = '\0';
            if (*p == ',') p++;
        } else {
            fields[count++] = p;
            while (*p && *p != ',' && *p != '\n' && *p != '\r') p++;
            if (*p) *p++ = '\0';
        }
    }
    return count;
}

// ============================================================================
// CSV Parser
// ============================================================================

static esp_err_t parse_csv_line_to_entry(char *line, chain_entry_t *entry) {
    memset(entry, 0, sizeof(chain_entry_t));
    
    char *fields[20];
    int field_count = split_csv_line(line, fields, 20);
    
    if (field_count < 1) return ESP_ERR_INVALID_ARG;
    
    const char *type = fields[0];
    
    // Control transfer
    if (strcmp(type, "control") == 0 && field_count >= 7) {
        entry->type = CHAIN_TYPE_CONTROL;
        entry->ctrl.bmRequestType = (uint8_t)parse_hex_or_dec(fields[1]);
        entry->ctrl.bRequest = (uint8_t)parse_hex_or_dec(fields[2]);
        entry->ctrl.wValue = (uint16_t)parse_hex_or_dec(fields[3]);
        entry->ctrl.wIndex = (uint16_t)parse_hex_or_dec(fields[4]);
        entry->ctrl.wLength = (uint16_t)parse_hex_or_dec(fields[5]);
        entry->ctrl.packetSize = (uint8_t)parse_hex_or_dec(fields[6]);
        
        if (field_count > 8 && fields[8] && *fields[8]) {
            entry->ctrl.dataLen = parse_hex_bytes(fields[8], entry->ctrl.data, sizeof(entry->ctrl.data));
        }
        if (field_count > 9) {
            entry->ctrl.deviceAddr = (uint8_t)parse_hex_or_dec(fields[9]);
        }
        if (field_count > 10 && fields[10]) {
            ESP_LOGI("CHAIN", "Parsing flags field[10]: '%s' (field_count=%d)", fields[10], field_count);
            if (strstr(fields[10], "noretry")) entry->flags |= CHAIN_FLAG_NO_RETRY;
            if (strstr(fields[10], "setuponly")) entry->flags |= CHAIN_FLAG_SETUP_ONLY;
            const char *ep = strstr(fields[10], "ep");
            if (ep) entry->ctrl.dataStageEp = (uint8_t)atoi(ep + 2);
            ESP_LOGI("CHAIN", "Parsed flags: 0x%02x (noretry=%d, setuponly=%d, dataStageEp=%d)", 
                     entry->flags,
                     (entry->flags & CHAIN_FLAG_NO_RETRY) ? 1 : 0,
                     (entry->flags & CHAIN_FLAG_SETUP_ONLY) ? 1 : 0,
                     entry->ctrl.dataStageEp);
        }
        return ESP_OK;
    }
    
    // Bulk/Interrupt/Isochronous IN
    if ((strcmp(type, "bulk_in") == 0 || strcmp(type, "interrupt_in") == 0 || strcmp(type, "iso_in") == 0) && field_count >= 2) {
        if (strcmp(type, "interrupt_in") == 0) {
            entry->type = CHAIN_TYPE_INTERRUPT_IN;
        } else if (strcmp(type, "iso_in") == 0) {
            entry->type = CHAIN_TYPE_ISO_IN;
        } else {
            entry->type = CHAIN_TYPE_BULK_IN;
        }
        entry->ep.endpoint = (uint8_t)parse_hex_or_dec(fields[1]);
        entry->ep.length = (field_count > 2) ? (uint16_t)parse_hex_or_dec(fields[2]) : 64;
        entry->ep.deviceAddr = (field_count > 3) ? (uint8_t)parse_hex_or_dec(fields[3]) : 0;
        entry->ep.timeout = (field_count > 4) ? (uint16_t)parse_hex_or_dec(fields[4]) : 1000;
        entry->ep.continuous = (field_count > 5) ? (uint8_t)parse_hex_or_dec(fields[5]) : 0;
        entry->ep.maxAttempts = (field_count > 6) ? (uint8_t)parse_hex_or_dec(fields[6]) : 1;
        return ESP_OK;
    }
    
    // Bulk/Interrupt/Isochronous OUT
    // Format: bulk_out,endpoint,data,deviceAddr,timeout
    if ((strcmp(type, "bulk_out") == 0 || strcmp(type, "interrupt_out") == 0 || strcmp(type, "iso_out") == 0) && field_count >= 3) {
        if (strcmp(type, "interrupt_out") == 0) {
            entry->type = CHAIN_TYPE_INTERRUPT_OUT;
        } else if (strcmp(type, "iso_out") == 0) {
            entry->type = CHAIN_TYPE_ISO_OUT;
        } else {
            entry->type = CHAIN_TYPE_BULK_OUT;
        }
        entry->ep.endpoint = (uint8_t)parse_hex_or_dec(fields[1]);
        // Data is in field 2 (hex string)
        if (fields[2] && *fields[2]) {
        entry->ep.dataLen = parse_hex_bytes(fields[2], entry->ep.data, sizeof(entry->ep.data));
        }
        entry->ep.length = entry->ep.dataLen;  // Length derived from data
        entry->ep.deviceAddr = (field_count > 3) ? (uint8_t)parse_hex_or_dec(fields[3]) : 0;
        entry->ep.timeout = (field_count > 4) ? (uint16_t)parse_hex_or_dec(fields[4]) : 1000;
        ESP_LOGI(TAG, "bulk_out: ep=%d, dataLen=%d, addr=%d, timeout=%d", 
                 entry->ep.endpoint, entry->ep.dataLen, entry->ep.deviceAddr, entry->ep.timeout);
        return ESP_OK;
    }
    
    // Waitfor
    if (strcmp(type, "waitfor") == 0 && field_count >= 2) {
        const char *wait_type = fields[1];
        
        if (strcmp(wait_type, "delay") == 0 && field_count >= 3) {
            entry->type = CHAIN_TYPE_WAIT_DELAY;
            entry->delay.duration_ms = (uint32_t)parse_hex_or_dec(fields[2]);
            return ESP_OK;
        }
        if (strcmp(wait_type, "gpio") == 0 && field_count >= 4) {
            entry->type = CHAIN_TYPE_WAIT_GPIO;
            entry->gpio.pin = (uint8_t)parse_hex_or_dec(fields[2]);
            entry->gpio.level = (uint8_t)parse_hex_or_dec(fields[3]);
            entry->gpio.timeout_sec = (field_count > 4) ? (uint16_t)parse_hex_or_dec(fields[4]) : 60;
            return ESP_OK;
        }
        if (strcmp(wait_type, "usb_reset") == 0) {
            entry->type = CHAIN_TYPE_WAIT_USB_RESET;
            return ESP_OK;
        }
        if (strcmp(wait_type, "vbus_cycle") == 0) {
            entry->type = CHAIN_TYPE_WAIT_VBUS_CYCLE;
            return ESP_OK;
        }
        if (strcmp(wait_type, "button") == 0) {
            entry->type = CHAIN_TYPE_WAIT_BUTTON;
            entry->button.timeout_sec = (field_count > 3) ? (uint16_t)parse_hex_or_dec(fields[3]) : 0;
            if (field_count > 2 && fields[2]) {
                strncpy(entry->button.label, fields[2], sizeof(entry->button.label) - 1);
            } else {
                strcpy(entry->button.label, "Press Continue");
            }
            return ESP_OK;
        }
        if (strcmp(wait_type, "webhook") == 0) {
            entry->type = CHAIN_TYPE_WAIT_WEBHOOK;
            entry->webhook.timeout_sec = (field_count > 3) ? (uint16_t)parse_hex_or_dec(fields[3]) : 300;
            if (field_count > 2 && fields[2]) {
                strncpy(entry->webhook.trigger_id, fields[2], sizeof(entry->webhook.trigger_id) - 1);
            }
            return ESP_OK;
        }
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    // Action
    if (strcmp(type, "action") == 0 && field_count >= 2) {
        const char *action_type = fields[1];
        
        if (strcmp(action_type, "copy") == 0 && field_count >= 8) {
            entry->type = CHAIN_TYPE_ACTION_COPY;
            entry->copy.fromReqNo = (int8_t)atoi(fields[3]);
            entry->copy.fromOffset = (uint8_t)parse_hex_or_dec(fields[4]);
            entry->copy.fromLength = (uint8_t)parse_hex_or_dec(fields[5]);
            entry->copy.toReqNo = (int8_t)atoi(fields[7]);
            
            const char *field_name = fields[6];
            if (strcmp(field_name, "wValue") == 0) entry->copy.toField = 0;
            else if (strcmp(field_name, "wIndex") == 0) entry->copy.toField = 1;
            else if (strcmp(field_name, "wLength") == 0) entry->copy.toField = 2;
            else if (strcmp(field_name, "dataBytes") == 0) entry->copy.toField = 3;
            else if (strcmp(field_name, "deviceAddr") == 0) entry->copy.toField = 4;
            
            return ESP_OK;
        }
        if (strcmp(action_type, "goto") == 0 && field_count >= 3) {
            entry->type = CHAIN_TYPE_ACTION_GOTO;
            entry->jump.targetIndex = (uint16_t)parse_hex_or_dec(fields[2]);
            return ESP_OK;
        }
        if (strcmp(action_type, "gpio_out") == 0 && field_count >= 4) {
            entry->type = CHAIN_TYPE_ACTION_GPIO_OUT;
            entry->gpio_out.pin = (uint8_t)parse_hex_or_dec(fields[2]);
            entry->gpio_out.level = (uint8_t)parse_hex_or_dec(fields[3]);
            return ESP_OK;
        }
        if (strcmp(action_type, "comment") == 0) {
            entry->type = CHAIN_TYPE_ACTION_COMMENT;
            return ESP_OK;
        }
        if (strcmp(action_type, "http") == 0 && field_count >= 3) {
            entry->type = CHAIN_TYPE_ACTION_HTTP;
            strncpy(entry->http.url, fields[2], sizeof(entry->http.url) - 1);
            entry->http.method = 0;  // Default GET
            if (field_count > 3 && strcasecmp(fields[3], "post") == 0) {
                entry->http.method = 1;
            }
            return ESP_OK;
        }
        if (strcmp(action_type, "config") == 0 && field_count >= 4) {
            entry->type = CHAIN_TYPE_ACTION_CONFIG;
            strncpy(entry->config.key, fields[2], sizeof(entry->config.key) - 1);
            strncpy(entry->config.value, fields[3], sizeof(entry->config.value) - 1);
            return ESP_OK;
        }
        // action,add32,<increment>,<entry_byte0>,<entry_byte1>,<entry_byte2>,<entry_byte3>[,<field>]
        // Adds increment to 32-bit value formed by field of 4 entries (with carry)
        // <field> is optional: wValue (default), wIndex, wLength
        if (strcmp(action_type, "add32") == 0 && field_count >= 7) {
            entry->type = CHAIN_TYPE_ACTION_ADD32;
            entry->add32.increment = (uint32_t)parse_hex_or_dec(fields[2]);
            entry->add32.entryIdx[0] = (uint8_t)parse_hex_or_dec(fields[3]);
            entry->add32.entryIdx[1] = (uint8_t)parse_hex_or_dec(fields[4]);
            entry->add32.entryIdx[2] = (uint8_t)parse_hex_or_dec(fields[5]);
            entry->add32.entryIdx[3] = (uint8_t)parse_hex_or_dec(fields[6]);
            // Parse optional field selector (default: wValue)
            entry->add32.field = 0; // default wValue
            if (field_count >= 8 && fields[7][0] != '\0') {
                if (strcasecmp(fields[7], "wIndex") == 0) {
                    entry->add32.field = 1;
                } else if (strcasecmp(fields[7], "wLength") == 0) {
                    entry->add32.field = 2;
                }
                // "wValue" or unrecognized stays 0
            }
            const char *field_names[] = {"wValue", "wIndex", "wLength"};
            ESP_LOGI(TAG, "add32: increment=0x%lx, entries=[%d,%d,%d,%d], field=%s",
                     (unsigned long)entry->add32.increment,
                     entry->add32.entryIdx[0], entry->add32.entryIdx[1],
                     entry->add32.entryIdx[2], entry->add32.entryIdx[3],
                     field_names[entry->add32.field]);
            return ESP_OK;
        }
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    // Condition
    if (strcmp(type, "condition") == 0 && field_count >= 8) {
        entry->type = CHAIN_TYPE_CONDITION;
        
        const char *op = fields[1];
        if (strcmp(op, "==") == 0) entry->cond.operator = CHAIN_OP_EQ;
        else if (strcmp(op, "!=") == 0) entry->cond.operator = CHAIN_OP_NE;
        else if (strcmp(op, "<") == 0) entry->cond.operator = CHAIN_OP_LT;
        else if (strcmp(op, ">") == 0) entry->cond.operator = CHAIN_OP_GT;
        else if (strcmp(op, "<=") == 0) entry->cond.operator = CHAIN_OP_LE;
        else if (strcmp(op, ">=") == 0) entry->cond.operator = CHAIN_OP_GE;
        
        entry->cond.aReqNo = (int8_t)atoi(fields[3]);
        entry->cond.aLength = (uint8_t)parse_hex_or_dec(fields[4]);
        entry->cond.bReqNo = (int8_t)atoi(fields[6]);
        entry->cond.bLength = (uint8_t)parse_hex_or_dec(fields[7]);
        
        if (field_count > 8) {
            const char *action = fields[8];
            if (strcmp(action, "skip") == 0) entry->cond.action = CHAIN_COND_SKIP;
            else if (strcmp(action, "break") == 0) entry->cond.action = CHAIN_COND_BREAK;
        }
        
        return ESP_OK;
    }
    
    return ESP_ERR_INVALID_ARG;
}

// ============================================================================
// Chain Execution
// ============================================================================

esp_err_t chain_engine_init(void) {
    if (!button_semaphore) {
        button_semaphore = xSemaphoreCreateBinary();
    }
    if (!webhook_semaphore) {
        webhook_semaphore = xSemaphoreCreateBinary();
    }
    if (!executor_job_ready) {
        executor_job_ready = xSemaphoreCreateBinary();
    }
    
    // Create executor task on Core 1
    if (!executor_task_handle) {
        BaseType_t ret = xTaskCreatePinnedToCore(
            usb_executor_task,
            "usb_executor",
            8192,
            NULL,
            5,                      // Priority (same as USB worker)
            &executor_task_handle,
            1                       // Core 1 (dedicated USB core)
        );
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create USB executor task");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "USB executor task created on Core 1");
    }
    
    ESP_LOGI(TAG, "Chain engine initialized");
    return ESP_OK;
}

static void store_response(int index, const chain_result_t *result) {
    int store_idx = index % MAX_STORED_RESPONSES;
    memcpy(&stored_responses[store_idx], result, sizeof(chain_result_t));
    if (index >= response_count) {
        response_count = index + 1;
    }
}

static chain_result_t* get_stored_response(int index) {
    if (index < 0) {
        index = response_count + index;
    }
    if (index >= 0 && index < response_count) {
        return &stored_responses[index % MAX_STORED_RESPONSES];
    }
    return NULL;
}

static esp_err_t execute_copy(const chain_entry_t *entry, chain_entry_t *chain, int chain_len, int current_idx) {
    chain_result_t *src = get_stored_response(entry->copy.fromReqNo);
    if (!src || src->data_len == 0) {
        ESP_LOGW(TAG, "Copy: source not found");
        return ESP_ERR_NOT_FOUND;
    }
    
    int target_idx = (entry->copy.toReqNo < 0) ? current_idx + 1 : entry->copy.toReqNo;
    if (target_idx < 0 || target_idx >= chain_len) {
        return ESP_ERR_INVALID_ARG;
    }
    
    chain_entry_t *target = &chain[target_idx];
    
    // Extract value (little-endian)
    uint32_t value = 0;
    int offset = entry->copy.fromOffset;
    int len = entry->copy.fromLength;
    
    if (offset + len > src->data_len) len = src->data_len - offset;
    if (len <= 0) return ESP_ERR_INVALID_ARG;
    
    for (int i = 0; i < len && i < 4; i++) {
        value |= ((uint32_t)src->data[offset + i]) << (i * 8);
    }
    
    ESP_LOGI(TAG, "Copy: %"PRIu32" from @%d:%d -> field %d", value, offset, len, entry->copy.toField);
    
    if (target->type == CHAIN_TYPE_CONTROL) {
        switch (entry->copy.toField) {
            case 0: target->ctrl.wValue = (uint16_t)value; break;
            case 1: target->ctrl.wIndex = (uint16_t)value; break;
            case 2: target->ctrl.wLength = (uint16_t)value; break;
            case 4: target->ctrl.deviceAddr = (uint8_t)value; break;
        }
    } else if (target->type == CHAIN_TYPE_BULK_IN || target->type == CHAIN_TYPE_INTERRUPT_IN || target->type == CHAIN_TYPE_ISO_IN) {
        switch (entry->copy.toField) {
            case 2: target->ep.length = (uint16_t)value; break;
            case 4: target->ep.deviceAddr = (uint8_t)value; break;
        }
    }
    
    return ESP_OK;
}

static esp_err_t execute_entry(chain_entry_t *entry, chain_result_t *result) {
    memset(result, 0, sizeof(chain_result_t));
    result->type = entry->type;
    
    switch (entry->type) {
        case CHAIN_TYPE_CONTROL: {
            // Populate result metadata for UI display
            result->bmRequestType = entry->ctrl.bmRequestType;
            result->bRequest = entry->ctrl.bRequest;
            result->wValue = entry->ctrl.wValue;
            result->wIndex = entry->ctrl.wIndex;
            result->wLength = entry->ctrl.wLength;
            result->packetSize = entry->ctrl.packetSize ? entry->ctrl.packetSize : 8;

            // Build config and use unified backend API
            usb_packet_config_t config = usb_packet_config_default();
            config.bmRequestType = entry->ctrl.bmRequestType;
            config.bRequest = entry->ctrl.bRequest;
            config.wValue = entry->ctrl.wValue;
            config.wIndex = entry->ctrl.wIndex;
            config.wLength = entry->ctrl.wLength;
            config.packet_size = entry->ctrl.packetSize ? entry->ctrl.packetSize : 8;
            config.device_addr = entry->ctrl.deviceAddr;
            config.data_stage_ep = entry->ctrl.dataStageEp;
            config.setup_only = (entry->flags & CHAIN_FLAG_SETUP_ONLY) != 0;
            config.max_nak_retries = (entry->flags & CHAIN_FLAG_NO_RETRY) ? 0 : -1;
            config.timeout_ms = 100;
            if (config.setup_only || config.max_nak_retries == 0) {
                ESP_LOGI("CHAIN", "Control transfer flags: setup_only=%d, max_nak_retries=%d (entry->flags=0x%02x)",
                         config.setup_only, config.max_nak_retries, entry->flags);
            }
            config.expect_response = (entry->ctrl.bmRequestType & 0x80) != 0;
            config.response_buffer = result->data;
            config.response_buffer_size = sizeof(result->data);
            
            size_t bytes_rx = 0;
            config.bytes_received = &bytes_rx;
            
            if (entry->ctrl.dataLen > 0) {
                memcpy(config.extra_data, entry->ctrl.data, entry->ctrl.dataLen);
                config.extra_data_len = entry->ctrl.dataLen;
            }
            
            // Unified API routes to correct backend (DWC2 or soft-host)
            esp_err_t ret = usb_backend_send_packet(&config);
            result->bytes_received = (uint16_t)bytes_rx;
            result->status = (ret == ESP_OK) ? 0 : 2;
            result->data_len = result->bytes_received;
            return ret;
        }
        
        case CHAIN_TYPE_BULK_IN:
        case CHAIN_TYPE_INTERRUPT_IN:
        case CHAIN_TYPE_ISO_IN: {
            size_t bytes_read = 0;
            size_t len = entry->ep.length ? entry->ep.length : 64;
            uint32_t timeout = entry->ep.timeout ? entry->ep.timeout : 1000;
            
            usb_endpoint_type_t ep_type = USB_EP_TYPE_BULK;
            if (entry->type == CHAIN_TYPE_INTERRUPT_IN) {
                ep_type = USB_EP_TYPE_INTERRUPT;
            } else if (entry->type == CHAIN_TYPE_ISO_IN) {
                ep_type = USB_EP_TYPE_ISOCHRONOUS;
            }
            
            // Auto data toggle: track per endpoint, alternate DATA0/DATA1
            uint8_t ep = entry->ep.endpoint & 0x0F;
            int8_t pid = ep_data_pid[ep];
            
            // Use new API with explicit data PID
            esp_err_t ret = usb_backend_endpoint_in_with_pid(
                    entry->ep.endpoint, entry->ep.deviceAddr, ep_type, 1,
                    pid, result->data, len, timeout, &bytes_read);
            
            // Toggle PID for next read on this endpoint (DATA0↔DATA1)
            if (ret == ESP_OK && bytes_read > 0) {
                ep_data_pid[ep] = (pid == 0) ? 2 : 0;
            }
            
            result->bytes_received = bytes_read;
            result->status = (ret == ESP_OK) ? 0 : 2;
            result->data_len = bytes_read;
            return ret;
        }
        
        case CHAIN_TYPE_BULK_OUT:
        case CHAIN_TYPE_INTERRUPT_OUT:
        case CHAIN_TYPE_ISO_OUT: {
            usb_endpoint_type_t ep_type = USB_EP_TYPE_BULK;
            if (entry->type == CHAIN_TYPE_INTERRUPT_OUT) {
                ep_type = USB_EP_TYPE_INTERRUPT;
            } else if (entry->type == CHAIN_TYPE_ISO_OUT) {
                ep_type = USB_EP_TYPE_ISOCHRONOUS;
            }
            uint32_t timeout = entry->ep.timeout ? entry->ep.timeout : 1000;
            
            // Unified API routes to correct backend (DWC2 or soft-host)
            esp_err_t ret = usb_backend_endpoint_out(
                entry->ep.endpoint, entry->ep.deviceAddr, ep_type, 1,
                entry->ep.data, entry->ep.dataLen, timeout);
            
            result->bytes_received = entry->ep.dataLen;
            result->status = (ret == ESP_OK) ? 0 : 2;
            return ret;
        }
        
        case CHAIN_TYPE_WAIT_DELAY:
            vTaskDelay(pdMS_TO_TICKS(entry->delay.duration_ms));
            result->status = 0;
            return ESP_OK;
        
        case CHAIN_TYPE_WAIT_GPIO: {
            gpio_reset_pin(entry->gpio.pin);
            gpio_set_direction(entry->gpio.pin, GPIO_MODE_INPUT);
            gpio_set_pull_mode(entry->gpio.pin, GPIO_PULLUP_ONLY);
            
            uint32_t start = xTaskGetTickCount();
            uint32_t timeout_ticks = pdMS_TO_TICKS(entry->gpio.timeout_sec * 1000);
            
            while ((xTaskGetTickCount() - start) < timeout_ticks) {
                if (chain_stop_requested) return ESP_ERR_TIMEOUT;
                if (gpio_get_level(entry->gpio.pin) == entry->gpio.level) {
                    result->status = 0;
                    return ESP_OK;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            result->status = 1;
            return ESP_ERR_TIMEOUT;
        }
        
        case CHAIN_TYPE_WAIT_USB_RESET:
            usb_backend_reset();
            vTaskDelay(pdMS_TO_TICKS(100));
            memset(ep_data_pid, 0, sizeof(ep_data_pid));  // Reset data toggle
            result->status = 0;
            return ESP_OK;
        
        case CHAIN_TYPE_WAIT_VBUS_CYCLE:
            ESP_LOGI(TAG, "VBUS power cycle — full device reset");
            dwc2_vbus_power_cycle_impl();
            memset(ep_data_pid, 0, sizeof(ep_data_pid));  // Reset data toggle
            result->status = 0;
            return ESP_OK;
        
        case CHAIN_TYPE_ACTION_COMMENT:
            result->status = 0;
            return ESP_OK;
        
        case CHAIN_TYPE_ACTION_GPIO_OUT: {
            gpio_reset_pin(entry->gpio_out.pin);
            gpio_set_direction(entry->gpio_out.pin, GPIO_MODE_OUTPUT);
            gpio_set_level(entry->gpio_out.pin, entry->gpio_out.level);
            ESP_LOGI(TAG, "GPIO %d -> %d", entry->gpio_out.pin, entry->gpio_out.level);
            result->status = 0;
            return ESP_OK;
        }
        
        case CHAIN_TYPE_WAIT_BUTTON: {
            ESP_LOGI(TAG, "Waiting for button: %s", entry->button.label);
            strncpy(button_label, entry->button.label, sizeof(button_label) - 1);
            waiting_for_button = true;
            button_pressed = false;
            
            // Clear semaphore and wait
            xSemaphoreTake(button_semaphore, 0);
            
            TickType_t timeout = (entry->button.timeout_sec > 0) 
                ? pdMS_TO_TICKS(entry->button.timeout_sec * 1000) 
                : portMAX_DELAY;
            
            // Wait for button press or stop request
            while (!button_pressed && !chain_stop_requested) {
                if (xSemaphoreTake(button_semaphore, pdMS_TO_TICKS(100)) == pdTRUE) {
                    break;
                }
                if (entry->button.timeout_sec > 0) {
                    timeout -= pdMS_TO_TICKS(100);
                    if (timeout == 0) break;
                }
            }
            
            waiting_for_button = false;
            result->status = button_pressed ? 0 : 1;
            return button_pressed ? ESP_OK : ESP_ERR_TIMEOUT;
        }
        
        case CHAIN_TYPE_WAIT_WEBHOOK: {
            ESP_LOGI(TAG, "Waiting for webhook: %s", entry->webhook.trigger_id);
            strncpy(webhook_pending_id, entry->webhook.trigger_id, sizeof(webhook_pending_id) - 1);
            waiting_for_webhook = true;
            webhook_triggered = false;
            
            // Clear semaphore and wait
            xSemaphoreTake(webhook_semaphore, 0);
            
            TickType_t timeout = pdMS_TO_TICKS(entry->webhook.timeout_sec * 1000);
            
            while (!webhook_triggered && !chain_stop_requested) {
                if (xSemaphoreTake(webhook_semaphore, pdMS_TO_TICKS(100)) == pdTRUE) {
                    break;
                }
                timeout -= pdMS_TO_TICKS(100);
                if (timeout == 0) break;
            }
            
            waiting_for_webhook = false;
            result->status = webhook_triggered ? 0 : 1;
            return webhook_triggered ? ESP_OK : ESP_ERR_TIMEOUT;
        }
        
        case CHAIN_TYPE_ACTION_HTTP: {
            ESP_LOGI(TAG, "HTTP %s: %s", entry->http.method ? "POST" : "GET", entry->http.url);
            
            esp_http_client_config_t config = {
                .url = entry->http.url,
                .method = entry->http.method ? HTTP_METHOD_POST : HTTP_METHOD_GET,
                .timeout_ms = 5000,
            };
            
            esp_http_client_handle_t client = esp_http_client_init(&config);
            if (!client) {
                result->status = 2;
                return ESP_ERR_NO_MEM;
            }
            
            esp_err_t err = esp_http_client_perform(client);
            int status_code = esp_http_client_get_status_code(client);
            esp_http_client_cleanup(client);
            
            if (err == ESP_OK && status_code >= 200 && status_code < 300) {
                ESP_LOGI(TAG, "HTTP OK, status=%d", status_code);
                result->status = 0;
                result->bytes_received = status_code;
                return ESP_OK;
            } else {
                ESP_LOGW(TAG, "HTTP failed: err=%d, status=%d", err, status_code);
                result->status = 2;
                result->bytes_received = status_code;
                return ESP_FAIL;
            }
        }
        
        default:
            result->status = 3;
            return ESP_OK;
    }
}

esp_err_t chain_execute_csv(const char *csv_data, size_t csv_len, 
                            chain_result_callback_t result_cb,
                            chain_wait_callback_t wait_cb,
                            void *user_data) {
    if (chain_running) {
        ESP_LOGW(TAG, "Chain already running");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!csv_data || csv_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Allocate entries array
    chain_entry_t *entries = calloc(CHAIN_MAX_ENTRIES, sizeof(chain_entry_t));
    if (!entries) {
        ESP_LOGE(TAG, "Failed to allocate entries");
        return ESP_ERR_NO_MEM;
    }
    
    // Make mutable copy
    char *csv_copy = malloc(csv_len + 1);
    if (!csv_copy) {
        free(entries);
        return ESP_ERR_NO_MEM;
    }
    memcpy(csv_copy, csv_data, csv_len);
    csv_copy[csv_len] = '\0';
    
    // Parse CSV
    int entry_count = 0;
    char *line = csv_copy;
    char *next_line;
    
    while (line && *line && entry_count < CHAIN_MAX_ENTRIES) {
        next_line = strchr(line, '\n');
        if (next_line) {
            *next_line = '\0';
            next_line++;
        }
        
        while (isspace((unsigned char)*line)) line++;
        if (*line == '\0' || *line == '#') {
            line = next_line;
            continue;
        }
        
        if (parse_csv_line_to_entry(line, &entries[entry_count]) == ESP_OK) {
            entry_count++;
        }
        
        line = next_line;
    }
    
    free(csv_copy);
    
    if (entry_count == 0) {
        free(entries);
        ESP_LOGE(TAG, "No valid entries");
        return ESP_ERR_INVALID_ARG;
    }

    // Disable auto-recovery for chains with oversized control packets (exploit mode)
    bool restore_auto_recovery = false;
    bool has_config_override = false;
    for (int i = 0; i < entry_count; i++) {
        if (entries[i].type == CHAIN_TYPE_ACTION_CONFIG) {
            if (strcasecmp(entries[i].config.key, "auto_recovery") == 0 ||
                strcasecmp(entries[i].config.key, "reset_on_retry") == 0) {
                has_config_override = true;
                break;
            }
        }
    }
    if (!has_config_override && usb_is_auto_recovery_enabled()) {
        for (int i = 0; i < entry_count; i++) {
            if (entries[i].type == CHAIN_TYPE_CONTROL && entries[i].ctrl.packetSize > 8) {
                usb_set_auto_recovery_enabled(false);
                restore_auto_recovery = true;
                break;
            }
        }
    }
    
    ESP_LOGI(TAG, "Executing %d entries", entry_count);
    
    // Reset state
    chain_running = true;
    chain_stop_requested = false;
    response_count = 0;
    
    // Execute
    chain_result_t result;
    int i = 0;
    int executed_count = 0;
    
    while (i < entry_count && !chain_stop_requested) {
        chain_entry_t *entry = &entries[i];
        
        if (entry->type == CHAIN_TYPE_ACTION_COPY) {
            execute_copy(entry, entries, entry_count, i);
            if (result_cb) {
                memset(&result, 0, sizeof(result));
                result.type = CHAIN_TYPE_ACTION_COPY;
                result.status = 0;
                result_cb(i, &result, user_data);
            }
            i++;
            continue;
        }
        
        if (entry->type == CHAIN_TYPE_ACTION_GOTO) {
            ESP_LOGI(TAG, "GOTO → entry %d", entry->jump.targetIndex);
            i = entry->jump.targetIndex;
            continue;
        }

        if (entry->type == CHAIN_TYPE_ACTION_ADD32) {
            // Helper to read the selected field from a control entry
            #define ADD32_GET_FIELD(e, f) ((f) == 1 ? (e).ctrl.wIndex : (f) == 2 ? (e).ctrl.wLength : (e).ctrl.wValue)
            #define ADD32_SET_FIELD(e, f, v) do { if ((f) == 1) (e).ctrl.wIndex = (v); else if ((f) == 2) (e).ctrl.wLength = (v); else (e).ctrl.wValue = (v); } while(0)
            
            uint8_t field = entry->add32.field;
            // Read 32-bit value from 4 entries' selected field (little-endian)
            uint32_t val = 0;
            for (int b = 0; b < 4; b++) {
                int idx = entry->add32.entryIdx[b];
                if (idx < entry_count && entries[idx].type == CHAIN_TYPE_CONTROL) {
                    val |= ((uint32_t)(ADD32_GET_FIELD(entries[idx], field) & 0xFF)) << (b * 8);
                }
            }
            uint32_t old_val = val;
            val += entry->add32.increment;
            // Write back individual bytes
            for (int b = 0; b < 4; b++) {
                int idx = entry->add32.entryIdx[b];
                if (idx < entry_count && entries[idx].type == CHAIN_TYPE_CONTROL) {
                    ADD32_SET_FIELD(entries[idx], field, (val >> (b * 8)) & 0xFF);
                }
            }
            const char *field_names[] = {"wValue", "wIndex", "wLength"};
            ESP_LOGI(TAG, "ADD32 [%s]: 0x%08lx + 0x%lx = 0x%08lx",
                     field_names[field],
                     (unsigned long)old_val, (unsigned long)entry->add32.increment, (unsigned long)val);
            
            #undef ADD32_GET_FIELD
            #undef ADD32_SET_FIELD
            if (result_cb) {
                memset(&result, 0, sizeof(result));
                result.type = CHAIN_TYPE_ACTION_ADD32;
                result.status = 0;
                result_cb(i, &result, user_data);
            }
            i++;
            continue;
        }

        if (entry->type == CHAIN_TYPE_ACTION_CONFIG) {
            const char *key = entry->config.key;
            const char *value = entry->config.value;
            if (strcasecmp(key, "auto_recovery") == 0 || strcasecmp(key, "reset_on_retry") == 0) {
                bool enable = true;
                if (strcasecmp(value, "0") == 0 || strcasecmp(value, "false") == 0 || strcasecmp(value, "off") == 0) {
                    enable = false;
                }
                usb_set_auto_recovery_enabled(enable);
            } else {
                ESP_LOGW(TAG, "Unknown config action: %s=%s", key, value);
            }
            if (result_cb) {
                memset(&result, 0, sizeof(result));
                result.type = CHAIN_TYPE_ACTION_CONFIG;
                result.status = 0;
                result_cb(i, &result, user_data);
            }
            i++;
            continue;
        }
        
        if (entry->type == CHAIN_TYPE_CONDITION) {
            chain_result_t *a = get_stored_response(entry->cond.aReqNo);
            bool cond_met = true;
            
            if (a) {
                uint32_t val_a = a->bytes_received;
                switch (entry->cond.operator) {
                    case CHAIN_OP_GT: cond_met = val_a > 0; break;
                    case CHAIN_OP_EQ: cond_met = val_a == 0; break;
                    case CHAIN_OP_NE: cond_met = val_a != 0; break;
                    default: break;
                }
                
                if (!cond_met) {
                    if (entry->cond.action == CHAIN_COND_SKIP) i++;
                    else if (entry->cond.action == CHAIN_COND_BREAK) i = entry_count;
                }
            }
            i++;
            continue;
        }
        
        // Notify frontend before waiting for button/webhook
        if (entry->type == CHAIN_TYPE_WAIT_BUTTON && wait_cb) {
            wait_cb(i, CHAIN_TYPE_WAIT_BUTTON, entry->button.label, user_data);
        } else if (entry->type == CHAIN_TYPE_WAIT_WEBHOOK && wait_cb) {
            wait_cb(i, CHAIN_TYPE_WAIT_WEBHOOK, entry->webhook.trigger_id, user_data);
        }
        
        execute_entry(entry, &result);
        store_response(executed_count, &result);
        executed_count++;
        
        if (result_cb) {
            result_cb(i, &result, user_data);
        }
        
        i++;
    }
    
    chain_running = false;
    if (restore_auto_recovery) {
        usb_set_auto_recovery_enabled(true);
    }
    free(entries);
    
    ESP_LOGI(TAG, "Chain complete (%d executed)", executed_count);
    return chain_stop_requested ? ESP_ERR_TIMEOUT : ESP_OK;
}

void chain_stop(void) {
    if (chain_running) {
        chain_stop_requested = true;
        ESP_LOGI(TAG, "Stop requested");
    }
}

bool chain_is_running(void) {
    return chain_running;
}

bool chain_is_waiting_button(void) {
    return waiting_for_button;
}

const char* chain_get_button_label(void) {
    return button_label;
}

void chain_button_continue(void) {
    if (waiting_for_button) {
        button_pressed = true;
        xSemaphoreGive(button_semaphore);
        ESP_LOGI(TAG, "Button continue received");
    }
}

bool chain_is_waiting_webhook(void) {
    return waiting_for_webhook;
}

const char* chain_get_webhook_trigger_id(void) {
    return webhook_pending_id;
}

void chain_webhook_trigger(const char *trigger_id) {
    if (waiting_for_webhook && trigger_id) {
        if (strcmp(webhook_pending_id, trigger_id) == 0 || webhook_pending_id[0] == '\0') {
            webhook_triggered = true;
            xSemaphoreGive(webhook_semaphore);
            ESP_LOGI(TAG, "Webhook triggered: %s", trigger_id);
        }
    }
}

// ============================================================================
// USB Executor Task Implementation
// ============================================================================

static void execute_bruteforce_job(const usb_job_bruteforce_t *params, chain_result_callback_t cb, chain_done_callback_t done_cb, void *user_data) {
    usb_packet_config_t config = usb_packet_config_default();
    config.timeout_ms = 100;
    config.expect_response = true;
    
    chain_result_t result = {0};
    result.type = CHAIN_TYPE_CONTROL;
    config.response_buffer = result.data;
    config.response_buffer_size = sizeof(result.data);
    
    size_t bytes_rx = 0;
    config.bytes_received = &bytes_rx;
    
    int index = 0;
    
    // Iterate through all field combinations
    for (uint16_t bmReq = params->bmRequestType.start; bmReq <= params->bmRequestType.end && !executor_stop_requested; bmReq++) {
        for (uint16_t bReq = params->bRequest.start; bReq <= params->bRequest.end && !executor_stop_requested; bReq++) {
            for (uint16_t wValHi = params->wValueHi.start; wValHi <= params->wValueHi.end && !executor_stop_requested; wValHi++) {
                for (uint16_t wValLo = params->wValueLo.start; wValLo <= params->wValueLo.end && !executor_stop_requested; wValLo++) {
                    for (uint16_t wIdxHi = params->wIndexHi.start; wIdxHi <= params->wIndexHi.end && !executor_stop_requested; wIdxHi++) {
                        for (uint16_t wIdxLo = params->wIndexLo.start; wIdxLo <= params->wIndexLo.end && !executor_stop_requested; wIdxLo++) {
                            for (uint16_t wLen = params->wLength.start; wLen <= params->wLength.end && !executor_stop_requested; wLen++) {
                                for (uint16_t pktSize = params->packetSize.start; pktSize <= params->packetSize.end && !executor_stop_requested; pktSize++) {
                                    
                                    config.bmRequestType = (uint8_t)bmReq;
                                    config.bRequest = (uint8_t)bReq;
                                    config.wValue = (wValHi << 8) | wValLo;
                                    config.wIndex = (wIdxHi << 8) | wIdxLo;
                                    config.wLength = wLen;
                                    config.packet_size = pktSize ? pktSize : 8;
                                    config.expect_response = (bmReq & 0x80) != 0;
                                    
                                    bytes_rx = 0;
                                    esp_err_t ret = usb_backend_send_packet(&config);
                                    
                                    result.bytes_received = (uint16_t)bytes_rx;
                                    result.data_len = bytes_rx;
                                    result.status = (ret == ESP_OK) ? 0 : 2;
                                    
                                    // Store USB params for display
                                    result.bmRequestType = config.bmRequestType;
                                    result.bRequest = config.bRequest;
                                    result.wValue = config.wValue;
                                    result.wIndex = config.wIndex;
                                    result.wLength = config.wLength;
                                    result.packetSize = config.packet_size;
                                    
                                    if (cb) {
                                        cb(index, &result, user_data);
                                    }
                                    
                                    index++;
                                    
                                    if (params->delay_ms > 0) {
                                        vTaskDelay(pdMS_TO_TICKS(params->delay_ms));
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

static void usb_executor_task(void *arg) {
    ESP_LOGI(TAG, "USB executor task started on Core %d", xPortGetCoreID());
    
    while (1) {
        // Wait for a job
        if (xSemaphoreTake(executor_job_ready, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "Executor: processing job type %d", current_job.type);
            executor_stop_requested = false;
            esp_err_t job_result = ESP_OK;
            
            switch (current_job.type) {
                case USB_JOB_CHAIN:
                    job_result = chain_execute_csv(
                        current_job.chain.csv_data,
                        current_job.chain.csv_len,
                        current_job.result_cb,
                        current_job.wait_cb,
                        current_job.user_data
                    );
                    // Free the CSV data
                    if (current_job.chain.csv_data) {
                        free(current_job.chain.csv_data);
                        current_job.chain.csv_data = NULL;
                    }
                    break;
                    
                case USB_JOB_BRUTEFORCE:
                    execute_bruteforce_job(&current_job.bruteforce, current_job.result_cb, current_job.done_cb, current_job.user_data);
                    break;
            }
            
            // Call done callback
            if (current_job.done_cb) {
                current_job.done_cb(executor_stop_requested ? ESP_ERR_TIMEOUT : job_result, current_job.user_data);
            }
            
            executor_busy = false;
            ESP_LOGI(TAG, "Executor: job complete");
        }
    }
}

// ============================================================================
// USB Executor Public API
// ============================================================================

esp_err_t usb_executor_submit_chain(const char *csv_data, size_t csv_len,
                                    chain_result_callback_t result_cb,
                                    chain_wait_callback_t wait_cb,
                                    chain_done_callback_t done_cb,
                                    void *user_data) {
    if (executor_busy) {
        ESP_LOGW(TAG, "Executor busy");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Copy CSV data
    char *csv_copy = malloc(csv_len + 1);
    if (!csv_copy) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(csv_copy, csv_data, csv_len);
    csv_copy[csv_len] = '\0';
    
    // Set up job
    memset(&current_job, 0, sizeof(current_job));
    current_job.type = USB_JOB_CHAIN;
    current_job.chain.csv_data = csv_copy;
    current_job.chain.csv_len = csv_len;
    current_job.result_cb = result_cb;
    current_job.wait_cb = wait_cb;
    current_job.done_cb = done_cb;
    current_job.user_data = user_data;
    
    executor_busy = true;
    xSemaphoreGive(executor_job_ready);
    
    ESP_LOGI(TAG, "Chain job submitted (%zu bytes)", csv_len);
    return ESP_OK;
}

esp_err_t usb_executor_submit_bruteforce(const usb_job_bruteforce_t *params,
                                         chain_result_callback_t result_cb,
                                         chain_done_callback_t done_cb,
                                         void *user_data) {
    if (executor_busy) {
        ESP_LOGW(TAG, "Executor busy");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Set up job
    memset(&current_job, 0, sizeof(current_job));
    current_job.type = USB_JOB_BRUTEFORCE;
    memcpy(&current_job.bruteforce, params, sizeof(usb_job_bruteforce_t));
    current_job.result_cb = result_cb;
    current_job.done_cb = done_cb;
    current_job.user_data = user_data;
    
    executor_busy = true;
    xSemaphoreGive(executor_job_ready);
    
    ESP_LOGI(TAG, "Bruteforce job submitted");
    return ESP_OK;
}

bool usb_executor_is_busy(void) {
    return executor_busy;
}

void usb_executor_stop(void) {
    executor_stop_requested = true;
    chain_stop_requested = true;
    ESP_LOGI(TAG, "Executor stop requested");
}
