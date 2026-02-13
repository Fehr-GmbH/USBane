# USBane Soft-Host Limitations

## Current Status: GPIO Bit-Bang USB Host Not Functional

The GPIO bit-bang Full-Speed USB host implementation (`soft_backend.c`) currently **does not work** due to hardware limitations of the ESP32-S3 GPIO subsystem. This document explains the technical challenges encountered.

## The Problem: SE1 Glitches During Line State Transitions

USB Full-Speed uses differential signaling on two lines (D+ and D-):
- **J state**: D+=HIGH, D-=LOW (idle state, represents logic '1' in NRZI)
- **K state**: D+=LOW, D-=HIGH (represents logic '0' in NRZI)  
- **SE0**: D+=LOW, D-=LOW (End-of-Packet marker)
- **SE1**: D+=HIGH, D-=HIGH (**Invalid state** - must never occur)

When transitioning between J and K states, **both pins must change simultaneously**. However, the ESP32-S3 GPIO exhibits asymmetric timing:
- **Rise time** (LOW→HIGH): ~10-15ns
- **Fall time** (HIGH→LOW): ~20-30ns

This asymmetry causes a brief SE1 glitch during every J↔K transition:
1. J→K transition: D+ starts falling, D- starts rising
2. D- reaches HIGH before D+ reaches LOW
3. For ~10-20ns, both lines are HIGH = **SE1 glitch**

### Measured Results (Saleae Logic Analyzer @ 100 MHz)

| Configuration | Bit Rate | SE1 Glitches | Notes |
|---------------|----------|--------------|-------|
| Direct write | 12 MHz | 35+ per packet | Correct timing, invalid signals |
| 4 NOP delay | 9.6 MHz | 35 per packet | Too slow, still has SE1 |
| 6 NOP delay | ~8.5 MHz | ~20 per packet | Too slow |
| 10 NOP delay | 7 MHz | 5 per packet | Far too slow (USB spec: 12 MHz ±0.25%) |

## Why This Matters

USB devices are designed to reject packets containing SE1 states. Even brief SE1 glitches during transitions can cause:
- Packet rejection by the device
- Loss of synchronization
- Complete communication failure


## Attempted Solutions

### 1. Dedicated GPIO (Single-Cycle Writes)
**Result**: Did not help. Even with single-cycle CPU writes to dedicated GPIO, the physical pin transition timing remains asymmetric.

### 2. SE0 Intermediate State
**Approach**: Write SE0 (both LOW), wait, then write target state.
**Result**: Reduces SE1 but slows bit rate below USB specification. Trade-off not viable.

### 3. Reduced Drive Strength
**Approach**: Use `GPIO_DRIVE_CAP_0` (weakest) to slow rise time.
**Result**: Minimal improvement. Fall time is still slower than rise time.

### 4. Different GPIO Pins
**Approach**: Tested GPIO 4/5 and 16/17.
**Result**: Same behavior on all pins.

## Potential Hardware Solutions (Not Yet Tested)

### 1. External Pull-Down Resistors (15kΩ)
Adding pull-down resistors to D+ and D- would help the falling edge transition faster by providing an active pull to ground.

### 2. Series Resistors (22-33Ω)
Adding series resistors would slow the rising edge, potentially matching it to the falling edge timing.

### 3. External Line Driver IC
Using a dedicated USB transceiver IC (like MAX3421E or USB3300) would provide proper differential signaling with matched rise/fall times.

### 4. Active Transistor Switching
Using NPN transistors to actively pull lines LOW (as described in [g3gg0.de USB-PD article](https://www.g3gg0.de/programming/esp32-pd-usb-pd-using-esp32-zigbee-crib/)) could provide faster, more consistent fall times.


## Technical Details

### ESP32-S3 Configuration
- CPU: 240 MHz (verified)
- GPIO: Dedicated GPIO bundle with single-cycle CPU writes
- Pins: GPIO 16 (D+), GPIO 17 (D-)
- Drive strength: GPIO_DRIVE_CAP_0 (weakest, ~5mA)

### USB Full-Speed Requirements
- Bit rate: 12 MHz ±0.25% (11.97-12.03 MHz)
- Rise/fall time: 4-20ns
- No SE1 states allowed during normal operation


## References

- [USB 2.0 Specification](https://www.usb.org/document-library/usb-20-specification)
- [ESP32-S3 Technical Reference Manual](https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_en.pdf)
- [Dedicated GPIO Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/dedic_gpio.html)
- [g3gg0.de: ESP32 USB-PD Bit-Banging](https://www.g3gg0.de/programming/esp32-pd-usb-pd-using-esp32-zigbee-crib/)

---

*Last updated: February 2026*
