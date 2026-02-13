/*
 * USBane - GPIO Soft-Host USB Backend (Full-Speed 12 Mbps)
 * 
 * Full-Speed USB host via GPIO bit-banging on ESP32-S3 @ 240MHz.
 * 
 * DERIVED WORK NOTICE:
 * This code is substantially derived from esp32_usb_soft_host by Dima Samsonov.
 * The original Low-Speed implementation was written by:
 *   Dima Samsonov @ Israel (sdima1357@gmail.com) - 3/2021
 *   Repository: https://github.com/sdima1357/esp32_usb_soft_host
 * 
 * The following components are derived from the original:
 *   - USB state machine architecture (fsm_Mashine, timerCallBack)
 *   - NRZI encoding/decoding (repack, parse_received_NRZI_buffer)
 *   - Packet building functions (pu_MSB, pu_Addr, pu_Cmd, CRC calculation)
 *   - Buffer structures and callback system
 * 
 * Modifications by Fehr GmbH (2026):
 *   - Full-Speed (12 Mbps) adaptation from original Low-Speed (1.5 Mbps)
 *   - ESP32-S3 dedicated GPIO for single-cycle bit-banging
 *   - SE1 glitch mitigation via transition-through-SE0
 *   - Extensive timing diagnostics
 * 
 * Copyright (C) 2021 Dmitry Samsonov - Original Low-Speed implementation
 * Copyright (C) 2026 Fehr GmbH - Full-Speed port and USBane integration
 * 
 * SPDX-License-Identifier: AGPL-3.0-or-later
 * 
 * As required by the original author:
 * "The original was written by Dima Samsonov @ Israel sdima1357@gmail.com on 3/2021"
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "driver/gpio.h"
#include "driver/dedic_gpio.h"
#include "hal/dedic_gpio_cpu_ll.h"
#include "sdkconfig.h"
#include "soc/soc.h"
#include "soc/rtc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#include "hal/gpio_hal.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_clk_tree.h"
#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"

#include "soft_backend.h"

// Dedicated GPIO for single-cycle USB bit-banging
// Bundle layout: channel 0 = D+, channel 1 = D-
// Output values: 0b01 = J (D+=1, D-=0), 0b10 = K (D+=0, D-=1), 0b00 = SE0
static dedic_gpio_bundle_handle_t usb_gpio_bundle = NULL;
static uint32_t dedic_out_mask = 0;
static uint32_t dedic_in_mask = 0;

// Dedicated GPIO states for single-cycle output
#define DEDIC_J   0b01  // D+=1, D-=0
#define DEDIC_K   0b10  // D+=0, D-=1
#define DEDIC_SE0 0b00  // D+=0, D-=0

// Channel assignments: D+ = channel 0 (bit 0), D- = channel 1 (bit 1)
#define DEDIC_DP_BIT  0b01  // D+ channel mask
#define DEDIC_DM_BIT  0b10  // D- channel mask

// Fastest GPIO control using ESP32-S3 PIE (Processor Instruction Extensions)
// EE.SET_BIT_GPIO_OUT imm[7:0] - Sets specified bits HIGH in GPIO_OUT
// EE.CLR_BIT_GPIO_OUT imm[7:0] - Clears specified bits LOW in GPIO_OUT
// These are single-cycle instructions, faster than write_mask
#define DEDIC_SET_DP()  __asm__ __volatile__("ee.set_bit_gpio_out 0x1")  // Set D+ HIGH (bit 0)
#define DEDIC_CLR_DP()  __asm__ __volatile__("ee.clr_bit_gpio_out 0x1")  // Clear D+ LOW (bit 0)
#define DEDIC_SET_DM()  __asm__ __volatile__("ee.set_bit_gpio_out 0x2")  // Set D- HIGH (bit 1)
#define DEDIC_CLR_DM()  __asm__ __volatile__("ee.clr_bit_gpio_out 0x2")  // Clear D- LOW (bit 1)

// SE1 avoidance macro - DISABLED for 12 MHz timing
// The GPIO fall time is slower than rise time, causing SE1 glitches during transitions.
// Adding delay (NOPs) reduces SE1 but slows bit rate below USB spec.
// See LIMITATIONS.md for details on the SE1 glitch issue.
// Current state: Direct writes for correct 12 MHz timing, SE1 glitches present
#define DEDIC_TRANSITION_VIA_SE0(target) dedic_gpio_cpu_ll_write_all(target)

#define T_START 	0b00000001
#define T_ACK   	0b01001011
#define T_NACK  	0b01011010
#define T_SOF		0b10100101
#define T_SETUP	0b10110100
#define T_DATA0	0b11000011
#define T_DATA1	0b11010010
#define T_DATA2	0b11100001
#define T_OUT		0b10000111
#define T_IN		0b10010110

#define T_ERR		0b00111100
#define T_PRE		0b00111100
#define T_NYET	0b01101001
#define T_STALL	0b01111000

// local non std
#define T_NEED_ACK   0b01111011
#define T_CHK_ERR    0b01111111

#define USB_LS_K  0
#define USB_LS_J  1
#define USB_LS_S  2

#define DEF_BUFF_SIZE 0x100

// something short like ACK 
#define SMALL_NO_DATA 36

// Full-Speed: 12 Mbps = 20 CPU cycles per bit @240MHz
// D+ high is idle (J state)
// USB Full-Speed = 12 MHz = 83.3ns/bit = 20 cycles @ 240MHz
// Loop overhead is ~13 cycles (GPIO write + array lookup + wait check + loop)
// Measured: with +=18, internal shows 18 cyc but Saleae shows 52.8ns = 18.9MHz
// This means wait loop isn't waiting because next_edge is always behind
// To get 20 cycles total: FS_CYCLES_PER_BIT must be at least 20
#define FS_CYCLES_PER_BIT 20

int TRANSMIT_TIME_DELAY = 0;
int TIME_MULT = 10;
int TM_OUT = 16;

#define TIME_SCALE (1024)

// ESP-IDF 5.x compatibility
#include "esp_cpu.h"
#include "hal/gpio_hal.h"
static inline uint32_t _getCycleCount32()
{
	uint32_t ccount = esp_cpu_get_cycle_count();
	return ccount;
}
static inline uint8_t _getCycleCount8d8(void) 
{
	uint32_t ccount = esp_cpu_get_cycle_count();
	return ccount >> 3;
}


// ESP32-S3 GPIO register macros for dedicated GPIO
// SET_I: Disable output driver, enable input
// SET_O: Enable output driver, disable input
// Note: GPIO.enable controls the output driver. The actual signal comes from dedicated GPIO
// via the GPIO matrix (func_out_sel_cfg was set by dedic_gpio_new_bundle).
#define SET_I     { GPIO.enable_w1tc = (1 << DP_PIN) | (1 << DM_PIN); PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[DP_PIN]); PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[DM_PIN]); }
#define SET_O     { GPIO.enable_w1ts = (1 << DP_PIN) | (1 << DM_PIN); }
// Full-Speed: J state = D+ high, D- low (idle state)
#define SE_J      { *snd[0][0] = (1 << DM_PIN); *snd[0][1] = (1 << DP_PIN); }
#define SE_0      { *snd[2][0] = (1 << DM_PIN); *snd[2][1] = (1 << DP_PIN); }

#define READ_BOTH_PINS (((GPIO.in & RD_MASK) << 8) >> RD_SHIFT)
volatile uint32_t *snd[4][2] = {
	{&GPIO.out_w1tc, &GPIO.out_w1ts},	
	{&GPIO.out_w1ts, &GPIO.out_w1tc},	
	{&GPIO.out_w1tc, &GPIO.out_w1tc},
	{&GPIO.out_w1tc, &GPIO.out_w1tc}
};

// Must be set each time with setPins
uint32_t DP_PIN;
uint32_t DM_PIN;

uint32_t DM_PIN_M; 
uint32_t DP_PIN_M; 
uint16_t M_ONE;
uint16_t P_ONE;
uint32_t RD_MASK;
uint32_t RD_SHIFT;

// Temporary buffers used inside low-level
volatile uint8_t received_NRZI_buffer_bytesCnt;
uint16_t received_NRZI_buffer[DEF_BUFF_SIZE];

volatile uint8_t transmit_bits_buffer_store_cnt;

// share same memory as received_NRZI_buffer
uint8_t* transmit_bits_buffer_store = (uint8_t*)&received_NRZI_buffer[0];

volatile uint8_t transmit_NRZI_buffer_cnt;
uint8_t transmit_NRZI_buffer[DEF_BUFF_SIZE];

volatile uint8_t decoded_receive_buffer_head;
volatile uint8_t decoded_receive_buffer_tail;
uint8_t decoded_receive_buffer[DEF_BUFF_SIZE];


typedef __packed struct
{
	uint8_t cmd;
	uint8_t addr;
	uint8_t eop;

	uint8_t  dataCmd;
	uint8_t  bmRequestType;
	uint8_t  bmRequest;
	uint16_t wValue;
	uint16_t wIndex;
	uint16_t wLen;
	
	// Extra data for oversized SETUP packets (exploit research)
	uint8_t  extra_data[248];
	size_t   extra_len;
} Req;

enum DeviceState { NOT_ATTACHED, ATTACHED, POWERED, DEFAULT, ADDRESS,
	PARSE_CONFIG, PARSE_CONFIG1, PARSE_CONFIG2, PARSE_CONFIG3,
	POST_ATTACHED, RESET_COMPLETE, POWERED_COMPLETE, DEFAULT_COMPL };

enum CallbackCmd { CB_CHECK, CB_RESET, CB_WAIT0, CB_POWER, CB_TICK,
	CB_2, CB_2Ack, CB_3, CB_4, CB_5, CB_6, CB_7, CB_8, CB_9, CB_WAIT1 };

typedef struct
{
	int isValid;
	int selfNum;
	int epCount;
	int cnt;
	uint8_t flags_new;
	uint8_t flags;
	
	uint32_t DP;
	uint32_t DM;
	volatile enum CallbackCmd cb_Cmd;
	volatile enum DeviceState fsm_state;
	volatile uint16_t wires_last_state;
	sDevDesc desc;
	sCfgDesc cfg;
	Req rq;

	int counterNAck;
	int counterAck;

	uint8_t descrBuffer[DEF_BUFF_SIZE];
	uint8_t descrBufferLen;

	volatile int bComplete;
	volatile int in_data_flip_flop;
	int cmdTimeOut;
	uint32_t ufPrintDesc;
	int numb_reps_errors_allowed;

	uint8_t acc_decoded_resp[DEF_BUFF_SIZE];
	uint8_t acc_decoded_resp_counter;
	
	int asckedReceiveBytes;
	int transmitL1Bytes;
	uint8_t transmitL1[DEF_BUFF_SIZE];
} sUsbContStruct;

sUsbContStruct * current;

void parseImmed(sUsbContStruct * pcurrent)
{
	static sCfgDesc cfg;
	static sIntfDesc sIntf;
	static HIDDescriptor hid[4];
	static sEPDesc epd;
	static int cfgCount = 0;
	static int sIntfCount = 0;
	static int hidCount = 0;

	int pos = 0;
#define STDCLASS 0x00
#define HIDCLASS 0x03
#define HUBCLASS 0x09
	pcurrent->epCount = 0;
	while (pos < pcurrent->descrBufferLen - 2)
	{
		uint8_t len  = pcurrent->descrBuffer[pos];
		uint8_t type = pcurrent->descrBuffer[pos + 1];
		if (len == 0) {
			pos = pcurrent->descrBufferLen;
		}
		if (pos + len <= pcurrent->descrBufferLen) {
			if (type == 0x2) {
				memcpy(&cfg, &pcurrent->descrBuffer[pos], len);
			} else if (type == 0x4) {
				memcpy(&sIntf, &pcurrent->descrBuffer[pos], len);
			} else if (type == 0x21) {
				hidCount++;
				int i = hidCount - 1;
				memcpy(&hid[i], &pcurrent->descrBuffer[pos], len);
			} else if (type == 0x5) {
				pcurrent->epCount++;
				memcpy(&epd, &pcurrent->descrBuffer[pos], len);
			}
		}
		pos += len;
	}
}


inline void restart()
{
	transmit_NRZI_buffer_cnt = 0;
}

void decoded_receive_buffer_clear()
{
	decoded_receive_buffer_tail = decoded_receive_buffer_head;
}

inline void decoded_receive_buffer_put(uint8_t val)
{
	decoded_receive_buffer[decoded_receive_buffer_head] = val;
	decoded_receive_buffer_head++;
}

uint8_t decoded_receive_buffer_get()
{
	return decoded_receive_buffer[decoded_receive_buffer_tail++];
}

uint8_t decoded_receive_buffer_size()
{
	return (uint8_t)(decoded_receive_buffer_head - decoded_receive_buffer_tail);
}

uint8_t cal5()
{
	uint8_t crcb;
	uint8_t rem;

	crcb = 0b00101;
	rem  = 0b11111;

	for (int k = 16; k < transmit_bits_buffer_store_cnt; k++)
	{
		int rb = (rem >> 4) & 1;
		rem = (rem << 1) & 0b11111;
		if (rb ^ (transmit_bits_buffer_store[k] & 1)) {
			rem ^= crcb;
		}
	}
	return (~rem) & 0b11111;
}

uint32_t cal16()
{
	uint32_t crcb;
	uint32_t rem;

	crcb = 0b1000000000000101;
	rem  = 0b1111111111111111;

	for (int k = 16; k < transmit_bits_buffer_store_cnt; k++)
	{
		int rb = (rem >> 15) & 1;
		rem = (rem << 1) & 0b1111111111111111;
		if (rb ^ (transmit_bits_buffer_store[k] & 1)) {
			rem ^= crcb;
		}
	}
	return (~rem) & 0b1111111111111111;
}

inline void seB(int bit)
{
	transmit_bits_buffer_store[transmit_bits_buffer_store_cnt++] = bit;
}

inline void pu_MSB(uint16_t msg, int N)
{
	for (int k = 0; k < N; k++) {
		seB(msg & (1 << (N - 1 - k)) ? 1 : 0);
	}
}

inline void pu_LSB(uint16_t msg, int N)
{
	for (int k = 0; k < N; k++) {
		seB(msg & (1 << (k)) ? 1 : 0);
	}
}


void repack()
{
	// NOTE: Do NOT reset transmit_NRZI_buffer_cnt here!
	// Multiple packets (SETUP token + DATA0) are built into the same buffer
	// before being sent together. The buffer is reset after transmission.
	
	// Full-Speed: bus idles at J, SYNC starts with K
	// No J preamble needed - the bus is already at J idle, and the first K
	// of SYNC provides the edge the receiver needs to lock on.
	int last = USB_LS_J;  // Bus was at J, so first 0-bit will toggle to K
	int cntOnes = 0;

	for (int k = 0; k < transmit_bits_buffer_store_cnt; k++)
	{
		if (transmit_bits_buffer_store[k] == 0)
		{
			if (last == USB_LS_J || last == USB_LS_S) {
				last = USB_LS_K;
			} else {
				last = USB_LS_J;
			}
			cntOnes = 0;
		}
		else if (transmit_bits_buffer_store[k] == 1)
		{
			cntOnes++;
			if (cntOnes == 6)
			{
				transmit_NRZI_buffer[transmit_NRZI_buffer_cnt] = last;
				transmit_NRZI_buffer_cnt++;
				if (last == USB_LS_J) {
					last = USB_LS_K;
				} else {
					last = USB_LS_J;
				}
				cntOnes = 0;
			}
			if (last == USB_LS_S) {
				last = USB_LS_J;
			}
		}
		transmit_NRZI_buffer[transmit_NRZI_buffer_cnt++] = last;
	}

	transmit_NRZI_buffer[transmit_NRZI_buffer_cnt++] = USB_LS_S;  // SE0 #1
	transmit_NRZI_buffer[transmit_NRZI_buffer_cnt++] = USB_LS_S;  // SE0 #2
	// Inter-packet idle: 10 J bits (matching Propeller reference)
	// Propeller uses delayAfter=10 between SETUP token and DATA0
	for (int i = 0; i < 10; i++) {
		transmit_NRZI_buffer[transmit_NRZI_buffer_cnt++] = USB_LS_J;
	}

	transmit_bits_buffer_store_cnt = 0;
}

uint8_t rev8(uint8_t j)
{
	uint8_t res = 0;
	for (int i = 0; i < 8; i++) {
		res <<= 1;
		res |= (j >> i) & 1;
	}
	return res;
}

uint16_t rev16(uint16_t j)
{
	uint16_t res = 0;
	for (int i = 0; i < 16; i++) {
		res <<= 1;
		res |= (j >> i) & 1;
	}
	return res;
}

int parse_received_NRZI_buffer()
{
	if (!received_NRZI_buffer_bytesCnt) return 0;
	
	uint32_t crcb;
	uint32_t rem;

	crcb = 0b1000000000000101;
	rem  = 0b1111111111111111;

	int res = 0;
	int cntOnes = 0;
	
	int terr = 0;
	uint8_t current_res = 0xfe;
	uint16_t prev = received_NRZI_buffer[0];
	int start = -1;
	uint8_t prev_smb = P_ONE;  // FS idle is D+ high

	for (int i = 1; i < received_NRZI_buffer_bytesCnt; i++)
	{
		uint16_t curr = (prev & 0xff00) + (((received_NRZI_buffer[i] - prev)) & 0xff);
		prev = received_NRZI_buffer[i];

		uint8_t smb = curr >> 8;
		int tm = (curr & 0xff);
		if (tm < 2 || (smb == 0))
		{
			terr += tm;
		}
		else
		{
			int delta = ((((curr + terr) & 0xff)) * TIME_MULT + TIME_SCALE / 2) / TIME_SCALE;
			
			for (int k = 0; k < delta; k++)
			{
				int incc = 1;
				{
					if (prev_smb != smb)
					{
						if (cntOnes != 6) {
							current_res = current_res * 2 + 0;
						} else {
							incc = 0;
						}
						cntOnes = 0;
					}
					else
					{
						current_res = current_res * 2 + 1;
						cntOnes++;
					}
					if (start >= 0) {
						start += incc;
					}
					if (current_res == 0x1 && start < 0) {
						start = 0;
					}
					if ((start & 0x7) == 0 && incc)
					{
						if (start == 8) {
							res = current_res;
						}
						decoded_receive_buffer_put(current_res);
						if (start > 8)
						{
							for (int bt = 0; bt < 8; bt++)
							{
								int rb = (rem >> 15) & 1;
								rem = (rem << 1) & 0b1111111111111111;
								if (rb ^ ((current_res >> (7 - bt)) & 1)) {
									rem ^= crcb;
								}
							}
						}
					}
				}
				prev_smb = smb;
			}
			terr = 0;
		}
	}
	rem &= 0b1111111111111111;
	if (rem == 0b1111111111111111) {
		return res;
	}
	if (rem == 0x800d) {
		return T_NEED_ACK;
	}
	else {
		return T_CHK_ERR;
	}
}


// ============================================================================
// Full-Speed TX + RX (combined critical section)
// ============================================================================

// Full-Speed sendOnly: transmit the NRZI buffer without waiting for a response.
// Used by ACK() and CB_8 status-stage packets.
void IRAM_ATTR sendOnly()
{
	if (transmit_NRZI_buffer_cnt == 0) {
		SET_I;
		return;
	}
	
	// Map USB_LS_* to dedicated GPIO values
	static const uint8_t dedic_map[4] = {DEDIC_K, DEDIC_J, DEDIC_SE0, DEDIC_SE0};
	
	// Write J BEFORE enabling output to avoid glitch on driver activation
	dedic_gpio_cpu_ll_write_all(DEDIC_J);
	SET_O;
	// J idle preamble (~2 bit times) before TX
	__asm__ __volatile__(
		"nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\n"
		"nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\n"
		"nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\n"
		"nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop"
	);
	
	// TX using INTERLEAVED writes to avoid SE1 glitches
	uint32_t irq_state = XTOS_DISABLE_ALL_INTERRUPTS;
	{
		register uint8_t *tx_ptr = transmit_NRZI_buffer;
		register uint8_t *tx_end_ptr = transmit_NRZI_buffer + transmit_NRZI_buffer_cnt;
		register uint32_t next_edge = esp_cpu_get_cycle_count() + FS_CYCLES_PER_BIT;
		register uint8_t prev_state = DEDIC_J;  // Bus was at J idle
		
		while (tx_ptr < tx_end_ptr) {
			register uint8_t new_state = dedic_map[*tx_ptr++];
			
			// Transition via SE0 to avoid SE1 glitches
			// GPIO fall time > rise time causes SE1 during J<->K
			if ((prev_state == DEDIC_J && new_state == DEDIC_K) ||
			    (prev_state == DEDIC_K && new_state == DEDIC_J)) {
				DEDIC_TRANSITION_VIA_SE0(new_state);
			} else {
				dedic_gpio_cpu_ll_write_all(new_state);
			}
			prev_state = new_state;
			
			while ((int32_t)(next_edge - esp_cpu_get_cycle_count()) > 0) {}
			next_edge += FS_CYCLES_PER_BIT;
		}
	}
	XTOS_RESTORE_INTLEVEL(irq_state);
	
	restart();
	SET_I;
}


// Full-Speed TX+RX - Combined critical section
// TX and RX must happen in ONE interrupt-free block to not miss device response
#define FS_RX_SAMPLES 2048
#define FS_DUR_PER_BIT 24

// RX sampling: store raw GPIO.in values for maximum speed, post-process later
static uint32_t fs_raw_samples[FS_RX_SAMPLES];
static uint8_t fs_rx_dminus[FS_RX_SAMPLES];

// Debug counters
static int fs_dbg_tx = 0;        // TX debug prints (never reset - GPIO DIAG/LOOPBACK print once)
static int fs_dbg_rx = 0;
static int fs_dbg_resp = 0;
static int fs_dbg_norx = 0;      // reset each FSM cycle to see "No resp" each attempt
static int fs_dbg_cycle = 0;     // tracks FSM cycles for periodic output

void fs_reset_debug_counters(void) {
	// Only reset the per-cycle counters, not the global ones
	fs_dbg_norx = 0;
	fs_dbg_cycle++;
}

void IRAM_ATTR sendRecieveNParse()
{
	// If nothing to send, don't do anything
	if (transmit_NRZI_buffer_cnt == 0) {
		received_NRZI_buffer_bytesCnt = 0;
		return;
	}
	
	// Trim trailing J idle bits from the LAST packet's EOP.
	// We keep the 2 SE0 + 1 J minimum, but remove extra J padding.
	// This minimizes the time we drive the bus after the last EOP,
	// giving us more time to capture the device's response.
	while (transmit_NRZI_buffer_cnt > 3 && 
	       transmit_NRZI_buffer[transmit_NRZI_buffer_cnt - 1] == USB_LS_J &&
	       transmit_NRZI_buffer[transmit_NRZI_buffer_cnt - 2] == USB_LS_J) {
		transmit_NRZI_buffer_cnt--;
	}
	
	// Save TX info for debug (before critical section)
	int total_tx_bits = transmit_NRZI_buffer_cnt;
	int saved_tx_len = transmit_NRZI_buffer_cnt > 200 ? 200 : transmit_NRZI_buffer_cnt;
	uint8_t saved_tx[200];
	for (int i = 0; i < saved_tx_len; i++) {
		saved_tx[i] = transmit_NRZI_buffer[i];
	}
	
	// Map USB_LS_* to dedicated GPIO values: channel 0 = D+, channel 1 = D-
	// USB_LS_K (0) -> D+=0, D-=1 -> 0b10 (DEDIC_K)
	// USB_LS_J (1) -> D+=1, D-=0 -> 0b01 (DEDIC_J)
	// USB_LS_S (2) -> D+=0, D-=0 -> 0b00 (DEDIC_SE0)
	static const uint8_t dedic_map[4] = {DEDIC_K, DEDIC_J, DEDIC_SE0, DEDIC_SE0};
	
	// Enable output and drive J idle before TX
	// USB spec requires the bus to be at J idle before starting a packet.
	// Write J BEFORE enabling output so there's no glitch when output driver activates
	dedic_gpio_cpu_ll_write_all(DEDIC_J);
	SET_O;
	// ~2 bit times of J idle (40 NOPs = ~40 cycles = ~166ns at 240MHz)
	__asm__ __volatile__(
		"nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\n"
		"nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\n"
		"nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\n"
		"nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop"
	);
	
	// Pre-enable input for RX phase (before critical section)
	PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[DP_PIN]);
	PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[DM_PIN]);
	
	// TX/RX timing variables
	uint32_t tx_start, tx_end, rx_start;
	
	uint32_t irq_state = XTOS_DISABLE_ALL_INTERRUPTS;
	
	// --- TX Phase using FAST SET/CLEAR instructions ---
	// For J<->K transitions, use dedicated set_bit/clr_bit instructions:
	//   J->K: D+ goes HIGH->LOW, D- goes LOW->HIGH
	//         First CLEAR D+ (so D+ falls), then SET D- (so D- rises)
	//         This ensures D+ is LOW before D- goes HIGH, avoiding SE1
	//   K->J: D+ goes LOW->HIGH, D- goes HIGH->LOW  
	//         First CLEAR D- (so D- falls), then SET D+ (so D+ rises)
	//         This ensures D- is LOW before D+ goes HIGH, avoiding SE1
	// The set/clear instructions are the fastest way to toggle GPIOs on ESP32-S3
	tx_start = esp_cpu_get_cycle_count();
	{
		register uint8_t *tx_ptr = transmit_NRZI_buffer;
		register uint8_t *tx_end_ptr = transmit_NRZI_buffer + transmit_NRZI_buffer_cnt;
		register uint32_t next_edge = tx_start + FS_CYCLES_PER_BIT;
		register uint8_t prev_state = DEDIC_J;  // Bus was at J idle
		
		while (tx_ptr < tx_end_ptr) {
			register uint8_t new_state = dedic_map[*tx_ptr++];
			
			// Transition via SE0 to avoid SE1 glitches
			if ((prev_state == DEDIC_J && new_state == DEDIC_K) ||
			    (prev_state == DEDIC_K && new_state == DEDIC_J)) {
				DEDIC_TRANSITION_VIA_SE0(new_state);
			} else {
				dedic_gpio_cpu_ll_write_all(new_state);
			}
			prev_state = new_state;
			
			while ((int32_t)(next_edge - esp_cpu_get_cycle_count()) > 0) {}
			next_edge += FS_CYCLES_PER_BIT;
		}
	}
	tx_end = esp_cpu_get_cycle_count();
	
	// --- Switch to RX ---
	// 1. Write SE0 to dedicated GPIO to stop driving
	// 2. Disable regular GPIO output driver
	dedic_gpio_cpu_ll_write_all(DEDIC_SE0);
	GPIO.enable_w1tc = (DP_PIN_M | DM_PIN_M);
	rx_start = esp_cpu_get_cycle_count();
	
	// --- RX Phase using regular GPIO.in ---
	{
		register volatile uint32_t *gpio_in_reg = &GPIO.in;
		register uint32_t *raw_dst = fs_raw_samples;
		register uint32_t *raw_end = fs_raw_samples + FS_RX_SAMPLES;
		
		while (raw_dst < raw_end) {
			*raw_dst++ = *gpio_in_reg;
		}
	}
	
	XTOS_RESTORE_INTLEVEL(irq_state);
	// === END CRITICAL SECTION ===
	
	// Post-process: extract D- from raw samples
	for (int i = 0; i < FS_RX_SAMPLES; i++) {
		fs_rx_dminus[i] = (fs_raw_samples[i] & DM_PIN_M) ? 1 : 0;
	}
	
	// Debug: show TX timing and RX samples (D- only: 0=low/J, 1=high/K)
	if (fs_dbg_tx < 8) {
		uint32_t tx_cycles = tx_end - tx_start;
		uint32_t turnaround_cycles = rx_start - tx_end;
		uint32_t cycles_per_bit = total_tx_bits > 0 ? tx_cycles / total_tx_bits : 0;
		
		// Calculate actual frequency: 240MHz / cycles_per_bit = bit_rate
		// USB FS spec: 12.000 MHz ±0.25% = 11.970 - 12.030 MHz
		if (fs_dbg_tx == 0) {
			float actual_mhz = (cycles_per_bit > 0) ? 240.0f / cycles_per_bit : 0;
			float error_pct = ((actual_mhz - 12.0f) / 12.0f) * 100.0f;
			printf("TX RATE: %.3f MHz (error: %+.2f%%, USB spec: ±0.25%%)\n", actual_mhz, error_pct);
			printf("TX TIME: %lu total cyc for %d bits = %lu ns\n", 
			       (unsigned long)tx_cycles, total_tx_bits, 
			       (unsigned long)(tx_cycles * 1000 / 240));
		}
		
		printf("TX: %d bits, %lu cyc/bit, turn=%lu cyc ", 
		       total_tx_bits, (unsigned long)cycles_per_bit,
		       (unsigned long)turnaround_cycles);
		for (int i = 0; i < saved_tx_len && i < 20; i++) {
			char c = saved_tx[i] == 0 ? 'K' : saved_tx[i] == 1 ? 'J' : '0';
			printf("%c", c);
		}
		printf("...\n");
		
		// Print FULL TX NRZI buffer for packet verification
		if (fs_dbg_tx == 0) {
			printf("TX FULL (%d): ", total_tx_bits);
			for (int i = 0; i < saved_tx_len; i++) {
				char c = saved_tx[i] == 0 ? 'K' : saved_tx[i] == 1 ? 'J' : saved_tx[i] == 2 ? '0' : '?';
				printf("%c", c);
			}
			printf("\n");
		}
		
		// Print RX bus state using both D+ and D-: J=D+hi/D-lo, K=D+lo/D-hi, 0=SE0, !=SE1
		printf("RX BUS[0-199]: ");
		for (int i = 0; i < 200 && i < FS_RX_SAMPLES; i++) {
			int dp = (fs_raw_samples[i] >> DP_PIN) & 1;
			int dm = (fs_raw_samples[i] >> DM_PIN) & 1;
			if (dp && !dm) printf("J");
			else if (!dp && dm) printf("K");
			else if (!dp && !dm) printf("0");
			else printf("!");
		}
		printf("\n");
		// Count bus states in full RX window & find last non-J sample
		int cnt_j = 0, cnt_k = 0, cnt_se0 = 0, cnt_se1 = 0;
		int last_non_j = -1;
		for (int i = 0; i < FS_RX_SAMPLES; i++) {
			int dp = (fs_raw_samples[i] >> DP_PIN) & 1;
			int dm = (fs_raw_samples[i] >> DM_PIN) & 1;
			if (dp && !dm) cnt_j++;
			else { 
				if (!dp && dm) cnt_k++;
				else if (!dp && !dm) cnt_se0++;
				else cnt_se1++;
				last_non_j = i;
			}
		}
		printf("RX stats: J=%d K=%d SE0=%d SE1=%d last_nonJ@%d (of %d)\n", 
		       cnt_j, cnt_k, cnt_se0, cnt_se1, last_non_j, FS_RX_SAMPLES);
		
		// Estimate RX sampling rate: 2048 samples in how many cycles?
		// At ~4 cycles/sample, 2048 samples = ~8192 cycles = ~34us = ~408 bit times
		if (fs_dbg_tx == 0) {
			// Count transitions in the first ~20 samples (our TX tail)
			int transitions = 0;
			for (int i = 1; i < 20 && i < FS_RX_SAMPLES; i++) {
				int prev_dp = (fs_raw_samples[i-1] >> DP_PIN) & 1;
				int prev_dm = (fs_raw_samples[i-1] >> DM_PIN) & 1;
				int curr_dp = (fs_raw_samples[i] >> DP_PIN) & 1;
				int curr_dm = (fs_raw_samples[i] >> DM_PIN) & 1;
				if (prev_dp != curr_dp || prev_dm != curr_dm) transitions++;
			}
			printf("RX DIAG: %d transitions in first 20 samples (expect ~5 from TX tail)\n", transitions);
			
			// Count ALL transitions in entire RX window — if device responds, there should be many more
			int total_trans = 0;
			for (int i = 1; i < FS_RX_SAMPLES; i++) {
				int prev_dp = (fs_raw_samples[i-1] >> DP_PIN) & 1;
				int prev_dm = (fs_raw_samples[i-1] >> DM_PIN) & 1;
				int curr_dp = (fs_raw_samples[i] >> DP_PIN) & 1;
				int curr_dm = (fs_raw_samples[i] >> DM_PIN) & 1;
				if (prev_dp != curr_dp || prev_dm != curr_dm) total_trans++;
			}
			// A response (~50 bits) at ~4 samples/bit would show ~50+ transitions
			printf("RX DIAG: %d total transitions in %d samples (expect 50+ if device responds)\n", 
			       total_trans, FS_RX_SAMPLES);
		}
		fs_dbg_tx++;
	}
	
	// Find device response using D- only (single-ended, like Propeller)
	// D- LOW = J (idle), D- HIGH = K (activity)  
	// Device response starts with SYNC: 7 zero-bits + 1 one-bit in NRZI
	// In NRZI on D-: alternating 1/0 pattern = KJKJKJKK
	// 
	// Skip first ~20 samples (TX tail leakage during GPIO turnaround)
	// Look for: sustained D- LOW (idle), then D- goes HIGH (first K of SYNC)
	// Require at least 8 consecutive LOW samples before accepting a HIGH
	int resp_start = -1;
	int idle_count = 0;
	
	for (int i = 10; i < FS_RX_SAMPLES - 100; i++) {
		if (fs_rx_dminus[i] == 0) {
			idle_count++;
		} else if (idle_count >= 8) {
			// D- went HIGH after at least 8 LOW samples (~2+ bit times idle)
			// Verify it's a real SYNC: next few samples should alternate
			// At ~4 samples/bit, one K bit = ~4 consecutive HIGH samples
			int high_count = 0;
			for (int j = i; j < i + 8 && j < FS_RX_SAMPLES; j++) {
				if (fs_rx_dminus[j]) high_count++;
			}
			if (high_count >= 3) {  // At least 3 out of 8 = real signal
				resp_start = i;
				break;
			}
			idle_count = 0;
		} else {
			idle_count = 0;
		}
	}
	
	if (resp_start < 0) {
		received_NRZI_buffer_bytesCnt = 0;
		restart();
		if (fs_dbg_norx < 8) {
			printf("FS RX: No resp\n");
			fs_dbg_norx++;
		}
		return;
	}
	
	if (fs_dbg_resp < 8) {
		printf("FS RESP @%d D-: ", resp_start);
		int show_len = 200;
		if (resp_start + show_len > FS_RX_SAMPLES) show_len = FS_RX_SAMPLES - resp_start;
		for (int i = resp_start; i < resp_start + show_len; i++) {
			printf("%c", fs_rx_dminus[i] ? '1' : '0');
		}
		printf("\n");
		fs_dbg_resp++;
	}
	
	// Find response end - sustained D- LOW (idle) after seeing activity
	int sample_cnt = FS_RX_SAMPLES;
	int idle_run = 0;
	int saw_activity = 0;
	for (int i = resp_start; i < FS_RX_SAMPLES; i++) {
		if (fs_rx_dminus[i]) {
			saw_activity = 1;
			idle_run = 0;
		} else {
			if (saw_activity) {
				idle_run++;
				if (idle_run > 20) {  // 20 samples of D- LOW = ~5 bit times idle = end of packet
					sample_cnt = i - idle_run + 1;
					break;
				}
			}
		}
	}
	
	// Convert D- samples to run-length encoded transitions for parse_received_NRZI_buffer()
	// The parser expects: high byte = bus state (0x01=J, 0x02=K), low byte = duration
	// D- HIGH (1) = K state = 0x02, D- LOW (0) = J state = 0x01
	int idx = 0;
	uint8_t prev = fs_rx_dminus[resp_start];
	int run_len = 1;
	
	for (int i = resp_start + 1; i < sample_cnt && idx < 128; i++) {
		uint8_t curr = fs_rx_dminus[i];
		if (curr == prev) {
			run_len++;
		} else {
			// Convert sample count to duration units (~4 samples per bit)
			int dur = (run_len * FS_DUR_PER_BIT + 2) / 4;
			if (dur < 1) dur = 1;
			if (dur > 255) dur = 255;
			// D- HIGH (1) = K = 0x02, D- LOW (0) = J = 0x01
			uint8_t bus_state = prev ? 0x02 : 0x01;
			received_NRZI_buffer[idx++] = (bus_state << 8) | dur;
			prev = curr;
			run_len = 1;
		}
	}
	if (run_len > 0 && idx < 128) {
		int dur = (run_len * FS_DUR_PER_BIT + 2) / 4;
		if (dur < 1) dur = 1;
		if (dur > 255) dur = 255;
		uint8_t bus_state = prev ? 0x02 : 0x01;
		received_NRZI_buffer[idx++] = (bus_state << 8) | dur;
	}
	
	received_NRZI_buffer_bytesCnt = idx;
	
	// Reset TX buffer for next packet build
	restart();
	
	if (fs_dbg_rx < 8 && idx > 0) {
		printf("FS RX parsed: %d transitions: ", idx);
		for (int i = 0; i < idx && i < 20; i++) {
			uint8_t st = received_NRZI_buffer[i] >> 8;
			uint8_t dur = received_NRZI_buffer[i] & 0xff;
			printf("%s:%d ", st == 0x02 ? "K" : "J", dur);
		}
		printf("\n");
		fs_dbg_rx++;
	}
}


int sendRecieve()
{
	sendRecieveNParse();
	return parse_received_NRZI_buffer();
}

void SOF()
{
	// Full-Speed: SOF frames are not needed for enumeration
	// Just ensure the TX buffer and bit buffer are clean for the next packet build
	restart();
	transmit_bits_buffer_store_cnt = 0;
}

// Send a Full-Speed SOF (Start of Frame) packet
// SOF packet: SYNC(8) + PID_SOF(8) + FrameNumber(11) + CRC5(5) = 32 bits
// This is TX-only, no RX expected. Sent without waiting for response.
static uint16_t sof_frame_number = 0;
static int sof_dbg_count = 0;
void IRAM_ATTR sendSOF()
{
	restart();
	transmit_bits_buffer_store_cnt = 0;
	
	pu_MSB(T_START, 8);       // SYNC
	pu_MSB(T_SOF, 8);        // PID = SOF (0xA5)
	pu_LSB(sof_frame_number & 0x7FF, 11); // 11-bit frame number, LSB first
	pu_MSB(cal5(), 5);       // CRC5
	repack();
	
	// Debug: dump first SOF packet
	if (sof_dbg_count == 0) {
		printf("SOF PKT (%d bits): ", transmit_NRZI_buffer_cnt);
		for (int i = 0; i < transmit_NRZI_buffer_cnt && i < 60; i++) {
			printf("%c", (transmit_NRZI_buffer[i] == USB_LS_J) ? 'J' : 
			             (transmit_NRZI_buffer[i] == USB_LS_K ? 'K' : 
			             (transmit_NRZI_buffer[i] == USB_LS_S ? '0' : '!')));
		}
		printf("\n");
		sof_dbg_count = 1;
	}
	
	// TX-only: drive the packet out without RX
	static const uint8_t dedic_map[4] = {DEDIC_K, DEDIC_J, DEDIC_SE0, DEDIC_SE0};
	
	// Write J BEFORE enabling output to avoid glitch on driver activation
	dedic_gpio_cpu_ll_write_all(DEDIC_J);
	SET_O;
	// J idle preamble (~2 bit times) before TX
	__asm__ __volatile__(
		"nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\n"
		"nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\n"
		"nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\n"
		"nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop"
	);
	
	// TX using INTERLEAVED writes to avoid SE1 glitches
	uint32_t irq_state = XTOS_DISABLE_ALL_INTERRUPTS;
	{
		register uint8_t *tx_ptr = transmit_NRZI_buffer;
		register uint8_t *tx_end_ptr = transmit_NRZI_buffer + transmit_NRZI_buffer_cnt;
		register uint32_t next_edge = esp_cpu_get_cycle_count() + FS_CYCLES_PER_BIT;
		register uint8_t prev_state = DEDIC_J;  // Bus was at J idle
		
		while (tx_ptr < tx_end_ptr) {
			register uint8_t new_state = dedic_map[*tx_ptr++];
			
			// Transition via SE0 to avoid SE1 glitches
			if ((prev_state == DEDIC_J && new_state == DEDIC_K) ||
			    (prev_state == DEDIC_K && new_state == DEDIC_J)) {
				DEDIC_TRANSITION_VIA_SE0(new_state);
			} else {
				dedic_gpio_cpu_ll_write_all(new_state);
			}
			prev_state = new_state;
			
			while ((int32_t)(next_edge - esp_cpu_get_cycle_count()) > 0) {}
			next_edge += FS_CYCLES_PER_BIT;
		}
	}
	XTOS_RESTORE_INTLEVEL(irq_state);
	SET_I;
	
	sof_frame_number = (sof_frame_number + 1) & 0x7FF;
	restart();
}


void pu_Addr(uint8_t cmd, uint8_t addr, uint8_t eop)
{
	pu_MSB(T_START, 8);
	pu_MSB(cmd, 8);
	pu_LSB(addr, 7);
	pu_LSB(eop, 4);
	pu_MSB(cal5(), 5);
	repack();
}

void pu_ShortCmd(uint8_t cmd)
{
	pu_MSB(T_START, 8);
	pu_MSB(cmd, 8);
	pu_MSB(0, 16);
	repack();
}

void pu_Cmd(uint8_t cmd, uint8_t bmRequestType, uint8_t bmRequest, uint16_t wValue, uint16_t wIndex, uint16_t wLen)
{
	pu_MSB(T_START, 8);
	pu_MSB(cmd, 8);
	pu_LSB(bmRequestType, 8);
	pu_LSB(bmRequest, 8);
	pu_LSB(wValue, 16);
	pu_LSB(wIndex, 16);
	pu_LSB(wLen, 16);
	pu_MSB(cal16(), 16);
	repack();
}

// Oversized SETUP packet for exploit research
void pu_Cmd_oversized(uint8_t cmd, uint8_t bmRequestType, uint8_t bmRequest,
                      uint16_t wValue, uint16_t wIndex, uint16_t wLen,
                      const uint8_t *extra_data, size_t extra_len)
{
	pu_MSB(T_START, 8);
	pu_MSB(cmd, 8);
	pu_LSB(bmRequestType, 8);
	pu_LSB(bmRequest, 8);
	pu_LSB(wValue, 16);
	pu_LSB(wIndex, 16);
	pu_LSB(wLen, 16);
	for (size_t i = 0; i < extra_len; i++) {
		pu_LSB(extra_data[i], 8);
	}
	pu_MSB(cal16(), 16);
	repack();
}

uint8_t ACK_BUFF[0x20];
int ACK_BUFF_CNT = 0;
void ACK()
{
	transmit_NRZI_buffer_cnt = 0;
	if (ACK_BUFF_CNT == 0)
	{
		pu_MSB(T_START, 8);
		pu_MSB(T_ACK, 8);
		repack();
		memcpy(ACK_BUFF, transmit_NRZI_buffer, transmit_NRZI_buffer_cnt);
		ACK_BUFF_CNT = transmit_NRZI_buffer_cnt;
	}
	else
	{
		memcpy(transmit_NRZI_buffer, ACK_BUFF, ACK_BUFF_CNT);
		transmit_NRZI_buffer_cnt = ACK_BUFF_CNT;
	}
	sendOnly();
}


// ============================================================================
// Timer callback and FSM
// ============================================================================

void timerCallBack()
{
	decoded_receive_buffer_clear();
	
	if (current->cb_Cmd == CB_CHECK)
	{
		SET_I;
		current->wires_last_state = READ_BOTH_PINS >> 8;
		// Full-Speed: P_ONE = D+ high = device connected
		current->bComplete = 1;
	}
	else if (current->cb_Cmd == CB_RESET)
	{
		// Full-Speed USB reset: drive SE0 for at least 10ms (USB spec minimum)
		// Use a timed SE0 directly here instead of relying on cmdTimeOut ticks
		// Write SE0 value BEFORE enabling output to avoid glitch
		dedic_gpio_cpu_ll_write_all(DEDIC_SE0);
		SET_O;
		ets_delay_us(50000); // 50ms SE0 — well above 10ms USB spec minimum
		SET_I;
		
		// Sample bus for ~2ms to detect HS chirp
		int chirp_k = 0, chirp_j = 0, chirp_se0 = 0;
		for (int i = 0; i < 2000; i++) {
			ets_delay_us(1);
			uint32_t pins = GPIO.in;
			int dp = (pins >> DP_PIN) & 1;
			int dm = (pins >> DM_PIN) & 1;
			if (!dp && dm) chirp_k++;
			else if (dp && !dm) chirp_j++;
			else if (!dp && !dm) chirp_se0++;
		}
		printf("POST-RESET BUS (2ms): K=%d J=%d SE0=%d (of 2000)\n", chirp_k, chirp_j, chirp_se0);
		if (chirp_k > 100) {
			printf("*** HS CHIRP DETECTED! Device wants High-Speed! ***\n");
		}
		
		// Wait 10ms for device to recover from reset
		ets_delay_us(10000);
		
		current->wires_last_state = READ_BOTH_PINS >> 8;
		printf("POST-RESET wire=0x%02x\n", current->wires_last_state);
		current->bComplete = 1;
		current->cb_Cmd = CB_CHECK; // skip CB_WAIT0, go directly to bComplete
	}
	else if (current->cb_Cmd == CB_WAIT0)
	{
		// Pre-reset delay — do NOT send SOFs here, device hasn't been reset yet
		// Just idle the bus
		if (current->cmdTimeOut > 0) {
			current->cmdTimeOut--;
		} else {
			SET_I;
			current->wires_last_state = READ_BOTH_PINS >> 8;
			current->bComplete = 1;
		}
	}
	else if (current->cb_Cmd == CB_WAIT1)
	{
		// Send SOF each tick to maintain 1ms frame timing (USB 2.0 requirement)
		sendSOF();
		if (current->cmdTimeOut > 0) {
			current->cmdTimeOut--;
		} else {
			SET_I;
			current->wires_last_state = READ_BOTH_PINS >> 8;
			current->bComplete = 1;
		}
	}
	else if (current->cb_Cmd == CB_POWER)
	{
		SET_I;
		
		static int power_attempts = 0;
		
		// Always send SOFs after reset — USB spec requires them
		for (int i = 0; i < 100; i++) {
			sendSOF();
			ets_delay_us(1000);
		}
		printf("CB_POWER: sent 100 SOFs (attempt %d)\n", power_attempts);
		
		// One-time deep bus probe: send an IN token and capture long RX
		if (power_attempts == 0) {
			static const uint8_t dedic_map_probe[4] = {DEDIC_K, DEDIC_J, DEDIC_SE0, DEDIC_SE0};
			
			// Build an IN token to addr 0, ep 0
			restart();
			transmit_bits_buffer_store_cnt = 0;
			pu_Addr(T_IN, 0, 0b0000);
			
			int in_len = transmit_NRZI_buffer_cnt;
			printf("PROBE IN token (%d bits): ", in_len);
			for (int i = 0; i < in_len && i < 60; i++) {
				printf("%c", transmit_NRZI_buffer[i] == USB_LS_K ? 'K' : 
				       transmit_NRZI_buffer[i] == USB_LS_J ? 'J' :
				       transmit_NRZI_buffer[i] == USB_LS_S ? '0' : '?');
			}
			printf("\n");
			
			// Trim trailing J idle from EOP
			while (in_len > 3 && 
			       transmit_NRZI_buffer[in_len - 1] == USB_LS_J &&
			       transmit_NRZI_buffer[in_len - 2] == USB_LS_J) {
				in_len--;
			}
			transmit_NRZI_buffer_cnt = in_len;
			
			// TX the IN token + capture long RX
			// Write J BEFORE enabling output to avoid glitch
			dedic_gpio_cpu_ll_write_all(DEDIC_J);
			SET_O;
			// J idle preamble (~2 bit times) before TX
			__asm__ __volatile__(
				"nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\n"
				"nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\n"
				"nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\n"
				"nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop"
			);
			PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[DP_PIN]);
			PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[DM_PIN]);
			#define PROBE_RX_SAMPLES 4096
			static uint32_t probe_samples[PROBE_RX_SAMPLES];
			
			uint32_t irq_state_probe = XTOS_DISABLE_ALL_INTERRUPTS;
			{
				// TX using FAST SET/CLEAR instructions to avoid SE1 glitches
				register uint8_t *tx_ptr = transmit_NRZI_buffer;
				register uint8_t *tx_end_ptr = transmit_NRZI_buffer + transmit_NRZI_buffer_cnt;
				register uint32_t next_edge = esp_cpu_get_cycle_count() + FS_CYCLES_PER_BIT;
				register uint8_t prev_state = DEDIC_J;  // Bus was at J idle
				
				while (tx_ptr < tx_end_ptr) {
					register uint8_t new_state = dedic_map_probe[*tx_ptr++];
					
					// Transition via SE0 to avoid SE1 glitches
					if ((prev_state == DEDIC_J && new_state == DEDIC_K) ||
					    (prev_state == DEDIC_K && new_state == DEDIC_J)) {
						DEDIC_TRANSITION_VIA_SE0(new_state);
					} else {
						dedic_gpio_cpu_ll_write_all(new_state);
					}
					prev_state = new_state;
					
					while ((int32_t)(next_edge - esp_cpu_get_cycle_count()) > 0) {}
					next_edge += FS_CYCLES_PER_BIT;
				}
				
				// Switch to RX
				dedic_gpio_cpu_ll_write_all(DEDIC_SE0);
				GPIO.enable_w1tc = (DP_PIN_M | DM_PIN_M);
				
				// Sample bus using regular GPIO.in
				register volatile uint32_t *gpio_in_p = &GPIO.in;
				register uint32_t *dst = probe_samples;
				register uint32_t *end = probe_samples + PROBE_RX_SAMPLES;
				while (dst < end) {
					*dst++ = *gpio_in_p;
				}
			}
			XTOS_RESTORE_INTLEVEL(irq_state_probe);
			SET_I;
			
			// Analyze the probe
			int p_j = 0, p_k = 0, p_se0 = 0, p_se1 = 0;
			int p_last_nonj = -1;
			for (int i = 0; i < PROBE_RX_SAMPLES; i++) {
				int dp = (probe_samples[i] >> DP_PIN) & 1;
				int dm = (probe_samples[i] >> DM_PIN) & 1;
				if (dp && !dm) p_j++;
				else if (!dp && dm) { p_k++; p_last_nonj = i; }
				else if (!dp && !dm) { p_se0++; p_last_nonj = i; }
				else { p_se1++; p_last_nonj = i; }
			}
			
			printf("PROBE RX (%d samples): J=%d K=%d SE0=%d SE1=%d last_nonJ@%d\n",
				PROBE_RX_SAMPLES, p_j, p_k, p_se0, p_se1, p_last_nonj);
			printf("PROBE RX[0-99]: ");
			for (int i = 0; i < 100 && i < PROBE_RX_SAMPLES; i++) {
				int dp = (probe_samples[i] >> DP_PIN) & 1;
				int dm = (probe_samples[i] >> DM_PIN) & 1;
				if (dp && !dm) printf("J");
				else if (!dp && dm) printf("K");
				else if (!dp && !dm) printf("0");
				else printf("!");
			}
			printf("\n");
			
			// Also show samples 100-199 in case response is delayed
			if (p_last_nonj > 99) {
				printf("PROBE RX[100-199]: ");
				for (int i = 100; i < 200 && i < PROBE_RX_SAMPLES; i++) {
					int dp = (probe_samples[i] >> DP_PIN) & 1;
					int dm = (probe_samples[i] >> DM_PIN) & 1;
					if (dp && !dm) printf("J");
					else if (!dp && dm) printf("K");
					else if (!dp && !dm) printf("0");
					else printf("!");
				}
				printf("\n");
			}
			
			restart();
		}
		
		power_attempts++;
		
		current->cmdTimeOut = 0;
		current->cb_Cmd = CB_WAIT1;
	}
	else if (current->cb_Cmd == CB_TICK)
	{
		sendSOF(); // Send a real SOF to keep device alive
		current->bComplete = 1;
	}
	else if (current->cb_Cmd == CB_3)
	{
		sendSOF();
		SOF(); // Reset TX buffer for packet build
		pu_Addr(current->rq.cmd, current->rq.addr, current->rq.eop);
		pu_Cmd(current->rq.dataCmd, current->rq.bmRequestType, current->rq.bmRequest,
		       current->rq.wValue, current->rq.wIndex, current->rq.wLen);
		int res = sendRecieve();
		if (res == T_ACK) {
			current->cb_Cmd = CB_4;
			current->numb_reps_errors_allowed = 8;
			return;
		} else {
			current->numb_reps_errors_allowed--;
			if (current->numb_reps_errors_allowed > 0) {
				return;
			} else {
				current->cb_Cmd = CB_TICK;
				current->bComplete = 1;
			}
		}
	}
	else if (current->cb_Cmd == CB_4)
	{
		sendSOF();
		SOF(); // Reset TX buffer for packet build
		pu_Addr(T_OUT, current->rq.addr, current->rq.eop);
		pu_MSB(T_START, 8);
		pu_MSB(T_DATA1, 8);
		for (int k = 0; k < current->transmitL1Bytes; k++) {
			pu_LSB(current->transmitL1[k], 8);
		}
		pu_MSB(cal16(), 16);
		repack();
		sendRecieveNParse();
		pu_Addr(T_IN, current->rq.addr, current->rq.eop);
		sendRecieveNParse();
		if (received_NRZI_buffer_bytesCnt < SMALL_NO_DATA && received_NRZI_buffer_bytesCnt > SMALL_NO_DATA / 4) {
			ACK();
		} else {
			current->numb_reps_errors_allowed--;
			if (current->numb_reps_errors_allowed > 0) {
				return;
			}
		}
		current->cb_Cmd = CB_TICK;
		current->bComplete = 1;
	}
	else if (current->cb_Cmd == CB_5)
	{
		sendSOF();
		SOF(); // Reset TX buffer for packet build
		pu_Addr(current->rq.cmd, current->rq.addr, current->rq.eop);
		if (current->rq.extra_len > 0) {
			pu_Cmd_oversized(current->rq.dataCmd, current->rq.bmRequestType, current->rq.bmRequest,
			                 current->rq.wValue, current->rq.wIndex, current->rq.wLen,
			                 current->rq.extra_data, current->rq.extra_len);
		} else {
			pu_Cmd(current->rq.dataCmd, current->rq.bmRequestType, current->rq.bmRequest,
			       current->rq.wValue, current->rq.wIndex, current->rq.wLen);
		}
		sendRecieveNParse();
		int res = parse_received_NRZI_buffer();
		if (res == T_ACK) {
			current->cb_Cmd = CB_6;
			current->in_data_flip_flop = 1;
			current->numb_reps_errors_allowed = 4;
			current->counterAck++;
			return;
		} else {
			current->counterNAck++;
			current->numb_reps_errors_allowed--;
			if (current->numb_reps_errors_allowed > 0) {
				current->acc_decoded_resp_counter = 0;
				return;
			} else {
				current->cb_Cmd = CB_TICK;
				current->bComplete = 1;
			}
		}
	}
	else if (current->cb_Cmd == CB_6)
	{
		sendSOF();
		SOF();
		pu_Addr(T_IN, current->rq.addr, current->rq.eop);
		sendRecieveNParse();
		// if receive something short
		if (current->asckedReceiveBytes == 0 && current->acc_decoded_resp_counter == 0 &&
		    received_NRZI_buffer_bytesCnt < SMALL_NO_DATA && received_NRZI_buffer_bytesCnt > SMALL_NO_DATA / 4)
		{
			ACK();
			current->cb_Cmd = CB_TICK;
			current->bComplete = 1;
			return;
		}
		int res = parse_received_NRZI_buffer();
		if (res == T_NEED_ACK)
		{
			if (decoded_receive_buffer_size() > 2)
			{
				decoded_receive_buffer_get();
				uint8_t sval = decoded_receive_buffer_get();
				if ((current->in_data_flip_flop & 1) == 1) {
					if (sval != T_DATA1) {
						current->cb_Cmd = CB_7;
						return;
					}
				} else {
					if (sval != T_DATA0) {
						current->cb_Cmd = CB_7;
						return;
					}
				}
				current->in_data_flip_flop++;
				int bytes = decoded_receive_buffer_size() - 2;
				for (int kk = 0; kk < bytes; kk++) {
					current->acc_decoded_resp[current->acc_decoded_resp_counter] = rev8(decoded_receive_buffer_get());
					current->acc_decoded_resp_counter++;
					current->asckedReceiveBytes--;
				}
				if (bytes <= 0) {
					current->acc_decoded_resp_counter = 0;
					current->asckedReceiveBytes = 0;
					current->cb_Cmd = CB_TICK;
					current->bComplete = 1;
				} else {
					current->cb_Cmd = CB_7;
					return;
				}
			}
			else
			{
				current->acc_decoded_resp_counter = 0;
				current->asckedReceiveBytes = 0;
				current->cb_Cmd = CB_TICK;
				current->bComplete = 1;
				return;
			}
		}
		else
		{
			current->numb_reps_errors_allowed--;
			if (current->numb_reps_errors_allowed > 0) {
				return;
			} else {
				current->cb_Cmd = CB_TICK;
				current->bComplete = 1;
			}
		}
	}
	else if (current->cb_Cmd == CB_7)
	{
		sendSOF();
		SOF();
		pu_Addr(T_IN, current->rq.addr, current->rq.eop);
		sendRecieveNParse();
		ACK();
		if (current->asckedReceiveBytes > 0) {
			current->cb_Cmd = CB_6;
			return;
		}
		current->cb_Cmd = CB_8;
	}
	else if (current->cb_Cmd == CB_8)
	{
		sendSOF();
		SOF();
		pu_Addr(T_OUT, current->rq.addr, current->rq.eop);
		pu_ShortCmd(T_DATA1);
		sendOnly();
		current->cb_Cmd = CB_TICK;
		current->bComplete = 1;
	}
	else if (current->cb_Cmd == CB_2Ack)
	{
		sendSOF();
		SOF();
		pu_Addr(T_IN, current->rq.addr, current->rq.eop);
		sendRecieveNParse();
		if (received_NRZI_buffer_bytesCnt < SMALL_NO_DATA / 2) {
			current->cb_Cmd = CB_TICK;
			current->bComplete = 1;
			return;
		}
		ACK();
		current->cb_Cmd = CB_TICK;
		current->bComplete = 1;
	}
	else if (current->cb_Cmd == CB_2)
	{
		sendSOF();
		SOF();
		pu_Addr(T_IN, current->rq.addr, current->rq.eop);
		sendRecieveNParse();
		if (received_NRZI_buffer_bytesCnt < SMALL_NO_DATA / 2) {
			current->cb_Cmd = CB_TICK;
			current->bComplete = 1;
			return;
		}
		int res = parse_received_NRZI_buffer();
		if (res == T_NEED_ACK)
		{
			if (decoded_receive_buffer_size() > 2)
			{
				decoded_receive_buffer_get();
				decoded_receive_buffer_get();
				int bytes = decoded_receive_buffer_size() - 2;
				for (int kk = 0; kk < bytes; kk++) {
					current->acc_decoded_resp[current->acc_decoded_resp_counter] = rev8(decoded_receive_buffer_get());
					current->acc_decoded_resp_counter++;
					current->asckedReceiveBytes--;
				}
			}
			current->asckedReceiveBytes = 0;
			current->cb_Cmd = CB_2Ack;
			return;
		}
		else
		{
			current->numb_reps_errors_allowed--;
			if (current->numb_reps_errors_allowed > 0) {
				return;
			} else {
				current->cb_Cmd = CB_TICK;
				current->bComplete = 1;
			}
		}
		current->cb_Cmd = CB_TICK;
		current->bComplete = 1;
		current->asckedReceiveBytes = 0;
	}
}


// ============================================================================
// Request functions
// ============================================================================

void Request(uint8_t cmd, uint8_t addr, uint8_t eop,
             uint8_t dataCmd, uint8_t bmRequestType, uint8_t bmRequest,
             uint16_t wValue, uint16_t wIndex, uint16_t wLen, uint16_t waitForBytes)
{
	current->bComplete = 0;
	current->rq.cmd = cmd;
	current->rq.addr = addr;
	current->rq.eop = eop;
	current->rq.dataCmd = dataCmd;
	current->rq.bmRequestType = bmRequestType;
	current->rq.bmRequest = bmRequest;
	current->rq.wValue = wValue;
	current->rq.wIndex = wIndex;
	current->rq.wLen = wLen;
	current->rq.extra_len = 0;
	current->numb_reps_errors_allowed = 4;
	current->asckedReceiveBytes = waitForBytes;
	current->acc_decoded_resp_counter = 0;
	current->cb_Cmd = CB_5;
}

void RequestOversized(uint8_t cmd, uint8_t addr, uint8_t eop,
                      uint8_t dataCmd, uint8_t bmRequestType, uint8_t bmRequest,
                      uint16_t wValue, uint16_t wIndex, uint16_t wLen,
                      uint16_t waitForBytes,
                      const uint8_t *extra_data, size_t extra_len)
{
	current->rq.cmd = cmd;
	current->rq.addr = addr;
	current->rq.eop = eop;
	current->rq.dataCmd = dataCmd;
	current->rq.bmRequestType = bmRequestType;
	current->rq.bmRequest = bmRequest;
	current->rq.wValue = wValue;
	current->rq.wIndex = wIndex;
	current->rq.wLen = wLen;
	if (extra_len > sizeof(current->rq.extra_data)) {
		extra_len = sizeof(current->rq.extra_data);
	}
	memcpy(current->rq.extra_data, extra_data, extra_len);
	current->rq.extra_len = extra_len;
	current->bComplete = 0;
	current->numb_reps_errors_allowed = 4;
	current->asckedReceiveBytes = waitForBytes;
	current->acc_decoded_resp_counter = 0;
	current->cb_Cmd = CB_5;
}

void RequestSend(uint8_t cmd, uint8_t addr, uint8_t eop,
                 uint8_t dataCmd, uint8_t bmRequestType, uint8_t bmRequest,
                 uint16_t wValue, uint16_t wIndex, uint16_t wLen,
                 uint16_t transmitL1Bytes, uint8_t* data)
{
	current->bComplete = 0;
	current->rq.cmd = cmd;
	current->rq.addr = addr;
	current->rq.eop = eop;
	current->rq.dataCmd = dataCmd;
	current->rq.bmRequestType = bmRequestType;
	current->rq.bmRequest = bmRequest;
	current->rq.wValue = wValue;
	current->rq.wIndex = wIndex;
	current->rq.wLen = wLen;
	current->rq.extra_len = 0;
	current->transmitL1Bytes = transmitL1Bytes;
	for (int k = 0; k < current->transmitL1Bytes; k++) {
		current->transmitL1[k] = data[k];
	}
	current->numb_reps_errors_allowed = 4;
	current->acc_decoded_resp_counter = 0;
	current->cb_Cmd = CB_3;
}

void RequestIn(uint8_t cmd, uint8_t addr, uint8_t eop, uint16_t waitForBytes)
{
	current->bComplete = 0;
	current->rq.cmd = cmd;
	current->rq.addr = addr;
	current->rq.eop = eop;
	current->numb_reps_errors_allowed = 4;
	current->asckedReceiveBytes = waitForBytes;
	current->acc_decoded_resp_counter = 0;
	current->cb_Cmd = CB_2;
}


// ============================================================================
// FSM (Finite State Machine) for USB enumeration
// ============================================================================

void fsm_Mashine()
{
	if (!current->bComplete) return;
	current->bComplete = 0;
	
	static int last_fsm_state = -1;
	if (current->fsm_state != last_fsm_state) {
		printf("FSM: state %d -> %d, cb_Cmd=%d, wire=0x%02x\n", 
		       last_fsm_state, current->fsm_state, current->cb_Cmd, current->wires_last_state);
		last_fsm_state = current->fsm_state;
	}
	
	if (current->fsm_state == 0)
	{
		current->epCount = 0;
		current->cb_Cmd = CB_CHECK;
		current->fsm_state = 1;
		// Reset debug counters so we see output for each enumeration attempt
		fs_reset_debug_counters();
		current->counterAck = 0;
		current->counterNAck = 0;
	}
	if (current->fsm_state == 1)
	{
		// Full-Speed: D+ high is idle state (P_ONE)
		if (current->wires_last_state == P_ONE)
		{
			current->cmdTimeOut = 100 + current->selfNum * 73;
			current->cb_Cmd = CB_WAIT0;
			current->fsm_state = 2;
			printf("FSM: Device detected (wire=0x%02x), advancing to state 2\n", current->wires_last_state);
		}
		else
		{
			current->fsm_state = 0;
			current->cb_Cmd = CB_CHECK;
		}
	}
	else if (current->fsm_state == 2)
	{
		printf("FSM: Sending USB RESET\n");
		current->cb_Cmd = CB_RESET;
		current->fsm_state = 3;
	}
	else if (current->fsm_state == 3)
	{
		current->cb_Cmd = CB_POWER;
		current->fsm_state = 4;
	}
	else if (current->fsm_state == 4)
	{
		Request(T_SETUP, ZERO_USB_ADDRESS, 0b0000, T_DATA0, 0x80, 0x6, 0x0100, 0x0000, 0x0012, 0x0012); 
		current->fsm_state = 5; 
	}
	else if (current->fsm_state == 5)
	{
		printf("FSM5: acc_decoded_resp_counter=%d (expected 0x12), ack=%d nack=%d\n",
		       current->acc_decoded_resp_counter, current->counterAck, current->counterNAck);
		if (current->acc_decoded_resp_counter == 0x12)
		{
			memcpy(&current->desc, current->acc_decoded_resp, 0x12);
			current->ufPrintDesc |= 1;
			printf("FSM5: Got device descriptor! VID=%04x PID=%04x\n", 
			       current->desc.idVendor, current->desc.idProduct);
		}
		else
		{
			printf("FSM5: No valid response, reps_allowed=%d\n", current->numb_reps_errors_allowed);
			if (current->numb_reps_errors_allowed <= 0) {
				current->fsm_state = 0; 
				return;
			}
		}
		Request(T_SETUP, ZERO_USB_ADDRESS, 0b0000, T_DATA0, 0x00, 0x5, 0x0000 + ASSIGNED_USB_ADDRESS, 0x0000, 0x0000, 0x0000);
		current->fsm_state = 6; 
	}
	else if (current->fsm_state == 6)
	{
		current->cmdTimeOut = 5; 
		current->cb_Cmd = CB_WAIT1;
		current->fsm_state = 7;
	}
	else if (current->fsm_state == 7)
	{
		Request(T_SETUP, ASSIGNED_USB_ADDRESS, 0b0000, T_DATA0, 0x80, 0x6, 0x0200, 0x0000, 0x0009, 0x0009); 
		current->fsm_state = 8; 
	}
	else if (current->fsm_state == 8)
	{
		if (current->acc_decoded_resp_counter == 0x9) {
			memcpy(&current->cfg, current->acc_decoded_resp, 0x9);
			current->ufPrintDesc |= 2;
			Request(T_SETUP, ASSIGNED_USB_ADDRESS, 0b0000, T_DATA0, 0x80, 0x6, 0x0200, 0x0000, current->cfg.wLength, current->cfg.wLength);
			current->fsm_state = 9;
		} else {
			current->fsm_state = 0;
			return;
		}
	}
	else if (current->fsm_state == 9)
	{
		if (current->acc_decoded_resp_counter == current->cfg.wLength) {
			current->ufPrintDesc |= 4;
			current->descrBufferLen = current->acc_decoded_resp_counter;
			memcpy(current->descrBuffer, current->acc_decoded_resp, current->descrBufferLen);
			parseImmed(current);
			current->fsm_state = 97; 
		} else {
			current->cmdTimeOut = 5;
			current->cb_Cmd = CB_WAIT1;
			current->fsm_state = 7;
		}
	}
	else if (current->fsm_state == 97)
	{
		Request(T_SETUP, ASSIGNED_USB_ADDRESS, 0b0000, T_DATA0, 0x00, 0x9, 0x0001, 0x0000, 0x0000, 0x0000);
		current->fsm_state = 98; 
	}
	else if (current->fsm_state == 98)
	{
		Request(T_SETUP, ASSIGNED_USB_ADDRESS, 0b0000, T_DATA0, 0x21, 0xa, 0x0000, 0x0000, 0x0000, 0x0000);
		current->fsm_state = 99; 
	}
	else if (current->fsm_state == 99)
	{
		if (current->flags_new != current->flags) {
			current->flags = current->flags_new;
			RequestSend(T_SETUP, ASSIGNED_USB_ADDRESS, 0b0000, T_DATA0, 0x21, 0x9, 0x0200, 0x0000, 0x0001, 0x0001, &current->flags);
		}
		current->fsm_state = 100; 
	}
	else if (current->fsm_state == 100)
	{
		led(0);
		RequestIn(T_IN, ASSIGNED_USB_ADDRESS, 1, 8);
		current->fsm_state = 101; 
	}
	else if (current->fsm_state == 101)
	{
		if (current->acc_decoded_resp_counter >= 1) {
			usbMess(current->selfNum * 4 + 0, current->acc_decoded_resp_counter, current->acc_decoded_resp);
			led(1);
		}
		if (current->epCount >= 2) {
			RequestIn(T_IN, ASSIGNED_USB_ADDRESS, 2, 8);
			current->fsm_state = 102; 
		} else {
			current->cmdTimeOut = 3; 
			current->cb_Cmd = CB_WAIT1;
			current->fsm_state = 104; 
		}
	}
	else if (current->fsm_state == 102)
	{
		if (current->acc_decoded_resp_counter >= 1) {
			usbMess(current->selfNum * 4 + 1, current->acc_decoded_resp_counter, current->acc_decoded_resp);
			led(1);
		}
		current->cmdTimeOut = 2; 
		current->cb_Cmd = CB_WAIT1;
		current->fsm_state = 104; 
	}
	else if (current->fsm_state == 104)
	{
		current->cmdTimeOut = 4; 
		current->cb_Cmd = CB_WAIT1;
		if (current->wires_last_state != P_ONE) {
			current->fsm_state = 0; 
			return;
		}
		current->fsm_state = 99; 
	}
	else
	{
		current->cmdTimeOut = 2; 
		current->cb_Cmd = CB_WAIT1;
		current->fsm_state = 0; 
	}
}


// ============================================================================
// Pin setup and initialization
// ============================================================================

void setPins(int DPPin, int DMPin)
{
	DP_PIN = DPPin;
	DM_PIN = DMPin;
	int diff = DPPin - DMPin;
	if (abs(diff) > 7) {
		printf("PIN DIFFERENCE MUST BE LESS 8!\n");
		exit(1);
	}
	int MIN_PIN = (DPPin < DMPin) ? DPPin : DMPin;
	
	DM_PIN_M = (1 << DMPin);
	DP_PIN_M = (1 << DPPin);
	RD_MASK = (1 << DPPin) | (1 << DMPin);
	
	RD_SHIFT = MIN_PIN;
	M_ONE = 1 << (DM_PIN - MIN_PIN);
	P_ONE = 1 << (DP_PIN - MIN_PIN);
}

sUsbContStruct current_usb[NUM_USB];

int checkPins(int dp, int dm)
{
	int diff = abs(dp - dm);
	if (diff > 7 || diff == 0) return 0;
	return 1;
}

int64_t get_system_time_us(void)
{
	return esp_timer_get_time();
}

void initStates(int DP0, int DM0, int DP1, int DM1, int DP2, int DM2, int DP3, int DM3)
{
	// Only use first port (DP0, DM0), ignore others since NUM_USB=1
	(void)DP1; (void)DM1; (void)DP2; (void)DM2; (void)DP3; (void)DM3;
	
	decoded_receive_buffer_head = 0;
	decoded_receive_buffer_tail = 0;
	transmit_bits_buffer_store_cnt = 0;
	
	for (int k = 0; k < NUM_USB; k++)
	{
		current = &current_usb[k];
		current->DP = DP0;
		current->DM = DM0;
		current->isValid = 0;
		if (checkPins(current->DP, current->DM))
		{
			printf("pins %lu %lu is OK!\n", (unsigned long)current->DP, (unsigned long)current->DM);
			current->selfNum = k;
			current->flags_new = 0x0;
			current->flags = 0x0;
			current->in_data_flip_flop = 0;
			current->bComplete = 1;
			current->cmdTimeOut = 0;
			current->ufPrintDesc = 0;
			current->cb_Cmd = CB_CHECK;
			current->fsm_state = 0;
			current->wires_last_state = 0;
			current->counterNAck = 0;
			current->counterAck = 0;
			current->epCount = 0;
			
			gpio_reset_pin(current->DP);
			gpio_reset_pin(current->DM);
			
			// ============================================================
			// SE1 GLITCH FIX: Use OPEN-DRAIN mode with external pull-ups
			// ============================================================
			// The SE1 glitch (both D+ and D- HIGH for 10-70ns during transitions)
			// happens because the ESP32's internal PMOS (pull-up) switches faster
			// than the NMOS (pull-down), causing one pin to go HIGH before the
			// other goes LOW.
			//
			// Open-drain mode solves this by:
			// - Active LOW: NMOS pulls down (fast, ~5ns)
			// - Passive HIGH: External pull-up resistor (slow, RC time constant)
			// 
			// With matched external pull-ups on D+ and D-, both pins will have
			// symmetric transition times, eliminating the SE1 glitch.
			//
			// HARDWARE REQUIRED: Add 1.5kΩ pull-up resistors from D+ and D- to 3.3V
			// ============================================================
			
#ifdef USB_OPEN_DRAIN_MODE
			printf("GPIO CONFIG: Using OPEN-DRAIN mode (needs 1.5k pull-ups to 3.3V!)\n");
			
			// Configure as open-drain output
			gpio_set_direction(current->DP, GPIO_MODE_OUTPUT_OD);
			gpio_set_direction(current->DM, GPIO_MODE_OUTPUT_OD);
			
			// Start with both HIGH (released/pulled up by external resistors)
			gpio_set_level(current->DP, 1);
			gpio_set_level(current->DM, 1);
			
			// Use strongest drive for fastest pull-down
			gpio_set_drive_capability(current->DP, GPIO_DRIVE_CAP_3);
			gpio_set_drive_capability(current->DM, GPIO_DRIVE_CAP_3);
			
			// Disable internal pull-down (we use external pull-ups)
			gpio_pulldown_dis(current->DP);
			gpio_pulldown_dis(current->DM);
			gpio_pullup_dis(current->DP);
			gpio_pullup_dis(current->DM);
#else
			printf("GPIO CONFIG: Using PUSH-PULL mode with lowest drive strength\n");
			
			// Push-pull mode with LOWEST drive strength to minimize asymmetry
			gpio_set_direction(current->DP, GPIO_MODE_OUTPUT);
			gpio_set_level(current->DP, 0);
			gpio_set_direction(current->DP, GPIO_MODE_INPUT);
			gpio_pulldown_en(current->DP);
			// GPIO_DRIVE_CAP_0 = ~5mA (lowest) - slower edges, more symmetric
			gpio_set_drive_capability(current->DP, GPIO_DRIVE_CAP_0);
			
			gpio_set_direction(current->DM, GPIO_MODE_OUTPUT);
			gpio_set_level(current->DM, 0);
			gpio_set_direction(current->DM, GPIO_MODE_INPUT);
			gpio_pulldown_en(current->DM);
			gpio_set_drive_capability(current->DM, GPIO_DRIVE_CAP_0);
#endif
			current->isValid = 1;
			
			// Setup pins and read initial state
			setPins(current->DP, current->DM);
			
			// Create Dedicated GPIO bundle for single-cycle USB bit-banging
			// This bypasses the GPIO matrix for minimum latency and jitter
			if (usb_gpio_bundle == NULL) {
				const int usb_gpios[] = {(int)current->DP, (int)current->DM}; // channel 0=D+, channel 1=D-
				dedic_gpio_bundle_config_t bundle_config = {
					.gpio_array = usb_gpios,
					.array_size = 2,
					.flags = {
						.in_en = 1,
						.out_en = 1,
					}
				};
				esp_err_t err = dedic_gpio_new_bundle(&bundle_config, &usb_gpio_bundle);
				if (err != ESP_OK) {
					printf("DEDIC GPIO: FATAL - failed to create bundle (err=%d)\n", err);
					current->isValid = 0;
					continue;
				}
				
				dedic_gpio_get_out_mask(usb_gpio_bundle, &dedic_out_mask);
				dedic_gpio_get_in_mask(usb_gpio_bundle, &dedic_in_mask);
				printf("DEDIC GPIO: bundle on Core %d, out_mask=0x%02lx, in_mask=0x%02lx\n",
				       xPortGetCoreID(), (unsigned long)dedic_out_mask, (unsigned long)dedic_in_mask);
				
				// Verify GPIO matrix routing — check what signal is routed to D+/D-
				// The func_out_sel_cfg register shows which peripheral signal drives the pad
				uint32_t dp_out_sel = GPIO.func_out_sel_cfg[current->DP].func_sel;
				uint32_t dm_out_sel = GPIO.func_out_sel_cfg[current->DM].func_sel;
				uint32_t dp_oen_sel = GPIO.func_out_sel_cfg[current->DP].oen_sel;
				uint32_t dm_oen_sel = GPIO.func_out_sel_cfg[current->DM].oen_sel;
				printf("DEDIC GPIO ROUTING (before fix): D+(GPIO%lu) func_sel=%lu oen_sel=%lu, D-(GPIO%lu) func_sel=%lu oen_sel=%lu\n",
				       (unsigned long)current->DP, (unsigned long)dp_out_sel, (unsigned long)dp_oen_sel,
				       (unsigned long)current->DM, (unsigned long)dm_out_sel, (unsigned long)dm_oen_sel);
				
				// FIX: Set oen_sel=1 so GPIO.enable controls output enable (not the peripheral)
				// This allows SET_O/SET_I to properly enable/disable the output driver
				GPIO.func_out_sel_cfg[current->DP].oen_sel = 1;
				GPIO.func_out_sel_cfg[current->DM].oen_sel = 1;
				
				// Verify the fix
				dp_oen_sel = GPIO.func_out_sel_cfg[current->DP].oen_sel;
				dm_oen_sel = GPIO.func_out_sel_cfg[current->DM].oen_sel;
				printf("DEDIC GPIO ROUTING (after fix): D+(GPIO%lu) oen_sel=%lu, D-(GPIO%lu) oen_sel=%lu (should be 1)\n",
				       (unsigned long)current->DP, (unsigned long)dp_oen_sel,
				       (unsigned long)current->DM, (unsigned long)dm_oen_sel);
				
				// Test dedicated GPIO: write J, K, SE0 and read back
				dedic_gpio_cpu_ll_write_all(DEDIC_J);
				ets_delay_us(1);
				uint32_t read_j = dedic_gpio_cpu_ll_read_in() & dedic_in_mask;
				dedic_gpio_cpu_ll_write_all(DEDIC_K);
				ets_delay_us(1);
				uint32_t read_k = dedic_gpio_cpu_ll_read_in() & dedic_in_mask;
				dedic_gpio_cpu_ll_write_all(DEDIC_SE0);
				ets_delay_us(1);
				uint32_t read_se0 = dedic_gpio_cpu_ll_read_in() & dedic_in_mask;
				dedic_gpio_cpu_ll_write_all(DEDIC_J); // back to idle
				
				printf("DEDIC GPIO TEST: J->0x%02lx K->0x%02lx SE0->0x%02lx (expect J=0x01, K=0x02, SE0=0x00)\n",
				       (unsigned long)read_j, (unsigned long)read_k, (unsigned long)read_se0);
			}
			
			// Test 1: Verify dedicated GPIO write is immediate (sample RIGHT AFTER write)
			{
				printf("DEDIC IMMEDIATE TEST: ");
				// Write initial J before enabling output
				dedic_gpio_cpu_ll_write_all(DEDIC_J);
				SET_O;
				uint32_t imm_samples[8];
				uint8_t imm_writes[8] = {DEDIC_J, DEDIC_K, DEDIC_J, DEDIC_K, DEDIC_J, DEDIC_K, DEDIC_J, DEDIC_K};
				
				uint32_t irq_imm = XTOS_DISABLE_ALL_INTERRUPTS;
				for (int i = 0; i < 8; i++) {
					dedic_gpio_cpu_ll_write_all(imm_writes[i]);
					// Read immediately after write (no delay)
					imm_samples[i] = GPIO.in;
				}
				XTOS_RESTORE_INTLEVEL(irq_imm);
				SET_I;
				
				printf("WRITE=");
				for (int i = 0; i < 8; i++) printf("%c", imm_writes[i] == DEDIC_K ? 'K' : 'J');
				printf(" READ=");
				for (int i = 0; i < 8; i++) {
					int dp = (imm_samples[i] >> DP_PIN) & 1;
					int dm = (imm_samples[i] >> DM_PIN) & 1;
					printf("%c", (dp && !dm) ? 'J' : ((!dp && dm) ? 'K' : ((!dp && !dm) ? '0' : '!')));
				}
				printf("\n");
			}
			
			// Test: Verify CPU clock is actually 240MHz
			{
				printf("CPU CLOCK VERIFY: ");
				int64_t us_start = esp_timer_get_time();
				uint32_t cyc_start = esp_cpu_get_cycle_count();
				ets_delay_us(1000); // Wait 1ms
				uint32_t cyc_end = esp_cpu_get_cycle_count();
				int64_t us_end = esp_timer_get_time();
				
				uint32_t cyc_elapsed = cyc_end - cyc_start;
				int64_t us_elapsed = us_end - us_start;
				float mhz = (float)cyc_elapsed / (float)us_elapsed;
				printf("%.1f MHz (%lu cyc in %lld us)\n", mhz, (unsigned long)cyc_elapsed, us_elapsed);
			}
			
			// Test 2: Actual TX loop timing test using esp_timer for ground truth
			{
				printf("TX LOOP TIMING TEST:\n");
				#define TIMING_TEST_N 1000
				
				// Create a test buffer (alternating J/K like SYNC)
				static uint8_t timing_buf[TIMING_TEST_N];
				for (int i = 0; i < TIMING_TEST_N; i++) timing_buf[i] = (i & 1) ? USB_LS_J : USB_LS_K;
				static const uint8_t timing_dedic_map[4] = {DEDIC_K, DEDIC_J, DEDIC_SE0, DEDIC_SE0};
				
				dedic_gpio_cpu_ll_write_all(DEDIC_J);
				SET_O;
				ets_delay_us(10);
				
				// Measure with cycle counter (using INTERLEAVED writes)
				uint32_t irq_tt = XTOS_DISABLE_ALL_INTERRUPTS;
				uint32_t tt_start = esp_cpu_get_cycle_count();
				{
					register uint8_t *tx_ptr = timing_buf;
					register uint8_t *tx_end_ptr = timing_buf + TIMING_TEST_N;
					register uint32_t next_edge = tt_start + FS_CYCLES_PER_BIT;
					register uint8_t prev_state = DEDIC_J;
					
					while (tx_ptr < tx_end_ptr) {
						register uint8_t new_state = timing_dedic_map[*tx_ptr++];
						if ((prev_state == DEDIC_J && new_state == DEDIC_K) ||
						    (prev_state == DEDIC_K && new_state == DEDIC_J)) {
							DEDIC_TRANSITION_VIA_SE0(new_state);
						} else {
							dedic_gpio_cpu_ll_write_all(new_state);
						}
						prev_state = new_state;
						while ((int32_t)(next_edge - esp_cpu_get_cycle_count()) > 0) {}
						next_edge += FS_CYCLES_PER_BIT;
					}
				}
				uint32_t tt_end = esp_cpu_get_cycle_count();
				XTOS_RESTORE_INTLEVEL(irq_tt);
				SET_I;
				
				uint32_t tt_cyc = tt_end - tt_start;
				float tt_per_bit = (float)tt_cyc / TIMING_TEST_N;
				float tt_mhz = 240.0f / tt_per_bit;
				float tt_ns_per_bit = tt_per_bit * (1000.0f / 240.0f);
				printf("  Cycle counter: %lu cyc for %d bits = %.2f cyc/bit = %.1f ns/bit = %.2f MHz (SE0 DELAY)\n",
				       (unsigned long)tt_cyc, TIMING_TEST_N, tt_per_bit, tt_ns_per_bit, tt_mhz);
				
				// Now measure the same thing with esp_timer for independent verification
				dedic_gpio_cpu_ll_write_all(DEDIC_J);
				SET_O;
				ets_delay_us(10);
				
				int64_t us_start = esp_timer_get_time();
				uint32_t irq_tt2 = XTOS_DISABLE_ALL_INTERRUPTS;
				{
					register uint8_t *tx_ptr = timing_buf;
					register uint8_t *tx_end_ptr = timing_buf + TIMING_TEST_N;
					register uint32_t next_edge = esp_cpu_get_cycle_count() + FS_CYCLES_PER_BIT;
					register uint8_t prev_state = DEDIC_J;
					
					while (tx_ptr < tx_end_ptr) {
						register uint8_t new_state = timing_dedic_map[*tx_ptr++];
						if ((prev_state == DEDIC_J && new_state == DEDIC_K) ||
						    (prev_state == DEDIC_K && new_state == DEDIC_J)) {
							DEDIC_TRANSITION_VIA_SE0(new_state);
						} else {
							dedic_gpio_cpu_ll_write_all(new_state);
						}
						prev_state = new_state;
						while ((int32_t)(next_edge - esp_cpu_get_cycle_count()) > 0) {}
						next_edge += FS_CYCLES_PER_BIT;
					}
				}
				XTOS_RESTORE_INTLEVEL(irq_tt2);
				int64_t us_end = esp_timer_get_time();
				SET_I;
				
				int64_t us_elapsed = us_end - us_start;
				float timer_ns_per_bit = (float)(us_elapsed * 1000) / TIMING_TEST_N;
				float timer_mhz = 1000.0f / timer_ns_per_bit;
				printf("  esp_timer:     %lld us for %d bits = %.1f ns/bit = %.2f MHz\n",
				       us_elapsed, TIMING_TEST_N, timer_ns_per_bit, timer_mhz);
				
				if (fabs(tt_mhz - timer_mhz) > 0.5f) {
					printf("  WARNING: Cycle counter and esp_timer disagree!\n");
				}
				
				// Also check: expected 83.3ns, actual timing
				float expected_ns = 1000.0f / 12.0f;  // 83.33ns for 12MHz
				float error_pct = ((timer_ns_per_bit - expected_ns) / expected_ns) * 100.0f;
				printf("  Target: %.1f ns/bit (12 MHz), Error: %+.1f%%\n", expected_ns, error_pct);
				
				vTaskDelay(pdMS_TO_TICKS(10));
			}
			
			// Full-Speed calibration
			uint32_t cpu_freq_hz = 0;
			esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &cpu_freq_hz);
			uint32_t freq_mhz = cpu_freq_hz / 1000000;
			
			printf("cpu freq = %lu MHz\n", (unsigned long)freq_mhz);
			
			// Full-Speed: 12 MHz USB
			// TIME_MULT converts cycle/8 deltas to bit counts
			// At 240MHz, 1 bit @ 12MHz = 20 cycles, /8 = 2.5
			// TIME_MULT = 1024 / 2.5 = 410
			TIME_MULT = (int)(TIME_SCALE / (freq_mhz / 8 / 12.0) + 0.5);
			TM_OUT = freq_mhz / 4;  // 60 iterations for 240MHz
			TRANSMIT_TIME_DELAY = 0;
			
			printf("TIME_MULT = %d (Full-Speed 12MHz)\n", TIME_MULT);
			printf("TM_OUT (RX timeout) = %d iterations\n", TM_OUT);
			printf("Full-Speed mode: Using inline NOPs for 12 Mbps timing\n");
			printf("Full-Speed detect: M_ONE=0x%02x P_ONE=0x%02x (wire should match P_ONE for FS device)\n", M_ONE, P_ONE);
		}
		else
		{
			printf("pins %lu %lu is Errors!\n", (unsigned long)current->DP, (unsigned long)current->DM);
		}
	}
}

void usbSetFlags(int _usb_num, uint8_t flags)
{
	if (_usb_num < NUM_USB && _usb_num >= 0) {
		current_usb[_usb_num].flags_new = flags;
	}
}

uint8_t usbGetFlags(int _usb_num)
{
	if (_usb_num < NUM_USB && _usb_num >= 0) {
		return current_usb[_usb_num].flags;
	}
	return 0;
}

void usb_process()
{
	for (int k = 0; k < NUM_USB; k++)
	{
		current = &current_usb[k];
		if (current->isValid) {
			setPins(current->DP, current->DM);
			timerCallBack();
			fsm_Mashine();
		}
	}
}

void printState()
{
	static int cntl = 0;
	cntl++;
	int ref = cntl % NUM_USB;
	sUsbContStruct * pcurrent = &current_usb[ref];
	if (!pcurrent->isValid) return;
	if ((cntl % 800) < NUM_USB) {
		printf("USB%d: Ack=%d Nack=%d %02x cb_Cmd=%d state=%d epCount=%d\n",
		       cntl % NUM_USB, pcurrent->counterAck, pcurrent->counterNAck,
		       pcurrent->wires_last_state, pcurrent->cb_Cmd, pcurrent->fsm_state, pcurrent->epCount);
	}
	if (pcurrent->ufPrintDesc & 1) {
		pcurrent->ufPrintDesc &= ~(uint32_t)1;
		printf("desc.bcdDevice       = %02x\n", pcurrent->desc.bcdDevice);
		printf("desc.iManufacturer   = %02x\n", pcurrent->desc.iManufacturer);
		printf("desc.iProduct        = %02x\n", pcurrent->desc.iProduct);
		printf("desc.iSerialNumber   = %02x\n", pcurrent->desc.iSerialNumber);
		printf("desc.bNumConfigurations = %02x\n", pcurrent->desc.bNumConfigurations);
	}
	if (pcurrent->ufPrintDesc & 2) {
		pcurrent->ufPrintDesc &= ~(uint32_t)2;
	}
	if (pcurrent->ufPrintDesc & 4) {
		pcurrent->ufPrintDesc &= ~(uint32_t)4;
		int pos = 0;
		printf("clear epCount %d self = %d\n", pcurrent->epCount, pcurrent->selfNum);
		while (pos < pcurrent->descrBufferLen - 2) {
			uint8_t len  = pcurrent->descrBuffer[pos];
			uint8_t type = pcurrent->descrBuffer[pos + 1];
			if (len == 0) {
				pos = pcurrent->descrBufferLen;
			}
			if (pos + len <= pcurrent->descrBufferLen) {
				printf("\n");
				if (type == 0x2) {
					sCfgDesc cfg;
					memcpy(&cfg, &pcurrent->descrBuffer[pos], len);
					printf("cfg.wLength         = %02x\n", cfg.wLength);
					printf("cfg.bNumIntf        = %02x\n", cfg.bNumIntf);
					printf("cfg.bCV             = %02x\n", cfg.bCV);
					printf("cfg.bMaxPower       = %d\n", cfg.bMaxPower);
				} else if (type == 0x4) {
					sIntfDesc sIntf;
					memcpy(&sIntf, &pcurrent->descrBuffer[pos], len);
				} else if (type == 0x21) {
					// HID descriptor
				} else if (type == 0x5) {
					sEPDesc epd;
					memcpy(&epd, &pcurrent->descrBuffer[pos], len);
					printf("pcurrent->epCount = %d\n", pcurrent->epCount);
					printf("epd.bEPAdd        = %02x\n", epd.bEPAdd);
					printf("epd.bAttr         = %02x\n", epd.bAttr);
					printf("epd.wPayLoad      = %02x\n", epd.wPayLoad);
					printf("epd.bInterval     = %02x\n", epd.bInterval);
				}
			}
			pos += len;
		}
	}
}


// ============================================================================
// Chain Engine Integration Functions
// ============================================================================

int usb_is_transfer_complete(int port)
{
	if (port < 0 || port >= NUM_USB) return 1;
	return current_usb[port].bComplete;
}

uint8_t* usb_get_response_buffer(int port, uint8_t *len)
{
	if (port < 0 || port >= NUM_USB) {
		if (len) *len = 0;
		return NULL;
	}
	if (len) *len = current_usb[port].acc_decoded_resp_counter;
	return current_usb[port].acc_decoded_resp;
}

void usb_get_debug_info(int port, int *cb_cmd, int *fsm_state, 
                        int *ack_count, int *nack_count, int *wire_state)
{
	if (port < 0 || port >= NUM_USB) {
		if (cb_cmd) *cb_cmd = -1;
		if (fsm_state) *fsm_state = -1;
		if (ack_count) *ack_count = 0;
		if (nack_count) *nack_count = 0;
		if (wire_state) *wire_state = 0;
		return;
	}
	sUsbContStruct *ctx = &current_usb[port];
	if (cb_cmd) *cb_cmd = ctx->cb_Cmd;
	if (fsm_state) *fsm_state = ctx->fsm_state;
	if (ack_count) *ack_count = ctx->counterAck;
	if (nack_count) *nack_count = ctx->counterNAck;
	if (wire_state) *wire_state = ctx->wires_last_state;
}

static int s_current_port = 0;

void usb_set_current_port(int port)
{
	if (port >= 0 && port < NUM_USB) {
		s_current_port = port;
		current = &current_usb[port];
	}
}

int soft_host_is_device_connected(int port)
{
	if (port < 0 || port >= NUM_USB) return 0;
	if (!current_usb[port].isValid) return 0;
	
	// Read live wire state
	// P_ONE means D+ is high (Full-Speed device connected)
	uint16_t wire_state = READ_BOTH_PINS >> 8;
	current_usb[port].wires_last_state = wire_state;
	
	return (wire_state == P_ONE);
}
