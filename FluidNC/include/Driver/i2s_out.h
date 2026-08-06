// Copyright (c) 2020 - Michiyasu Odaki
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "Driver/fluidnc_gpio.h"

#define I2S_OUT_NUM_BITS 32

// #    define I2SO(n) (I2S_OUT_PIN_BASE + n)

// The longest pulse that we allow when using I2S.  It is affected by the
// FIFO depth and could probably be a bit longer, but empirically this is
// enough for all known stepper drivers.
const uint32_t I2S_MAX_USEC_PER_PULSE = 20;

typedef struct {
    /*
        I2S bitstream (32-bits): Transfers from MSB(bit31) to LSB(bit0) in sequence
        ------------------time line------------------------>
             Left Channel                    Right Channel
        ws   ________________________________~~~~...
        bck  _~_~_~_~_~_~_~_~_~_~_~_~_~_~_~_~_~_~...
        data vutsrqponmlkjihgfedcba9876543210
             XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
                                             ^
                                Latches the X bits when ws is switched to High
        If I2S_OUT_PIN_BASE is set to 128,
        bit0:Expanded GPIO 128, 1: Expanded GPIO 129, ..., v: Expanded GPIO 159
    */
    pinnum_t ws_pin;
    pinnum_t bck_pin;
    pinnum_t data_pin;
    uint32_t pulse_period;  // aka step rate.
    uint32_t init_val;
    uint32_t min_pulse_us;
    uint32_t fifo_threshold;
    uint32_t fifo_reload;
    int8_t   ws_drive_strength;
    int8_t   bck_drive_strength;
    int8_t   data_drive_strength;
} i2s_out_init_t;

/*
  Initialize I2S out by parameters.
  return -1 ... already initialized
*/
void i2s_out_init(i2s_out_init_t* init_param);

/*
  Read a bit state from the internal pin state var.
  pin: expanded pin No. (0..31)
*/
uint8_t i2s_out_read(pinnum_t pin);

/*
   Set a bit in the internal pin state var. (not written electrically)
   pin: expanded pin No. (0..31)
   val: bit value(0 or not 0)
*/
void i2s_out_write(pinnum_t pin, uint8_t val);

/*
  Dynamically delay until the Shift Register Pin changes
  according to the current I2S processing state and mode.
 */
void i2s_out_delay();

// Optional continuous step stream overlaid on the normal I2S step engine.
// Rate ramps and position accounting stay in foreground code.  The ISR only
// consumes the precomputed rate and counts emitted pulses.
bool i2s_out_aux_step_start(pinnum_t step_pin,
                            bool step_invert,
                            pinnum_t dir_pin,
                            bool dir_level,
                            uint32_t initial_rate_millihz);
void i2s_out_aux_step_set_rate(uint32_t target_rate_millihz);
void i2s_out_aux_step_stop(bool immediate);
bool i2s_out_aux_step_active();
uint32_t i2s_out_aux_step_current_rate_millihz();
uint32_t i2s_out_aux_step_pulse_count();

typedef struct {
    uint32_t fifo_threshold;
    uint32_t fifo_reload;
    uint32_t underruns;
    uint32_t max_isr_gap_us;
    uint32_t max_isr_duration_us;
    uint32_t requested_rate_millihz;
    uint32_t emitted_rate_millihz;
    uint32_t emitted_pulses;
    bool     aux_faulted;
} i2s_out_diagnostics_t;

void i2s_out_get_diagnostics(i2s_out_diagnostics_t* diagnostics);
bool i2s_out_aux_step_take_fault();

/*
   Reference: "ESP32 Technical Reference Manual" by Espressif Systems
     https://www.espressif.com/sites/default/files/documentation/esp32_technical_reference_manual_en.pdf
 */

#ifdef __cplusplus
}
#endif
