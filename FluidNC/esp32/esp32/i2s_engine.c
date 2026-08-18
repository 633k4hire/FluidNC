// Copyright (c) 2024 -	Mitch Bradley
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

// Stepping engine that uses the I2S FIFO.  An interrupt service routine runs when
// the FIFO is below a set threshold.  The ISR pushes samples into the FIFO, representing
// step pulses and inter-pulse delays.  There are variables for the value to push for
// a pulse, the number of samples for that pulse, the value to push to the delay, and
// the number of samples for the delay.  When the delay is done, the ISR calls the Stepper
// pulse_func to determine the new values of those variables. The FIFO lets the ISR stay
// just far enough ahead so the information is always ready, but not so far ahead to cause
// latency problems.

#include "Driver/step_engine.h"
#include "Driver/i2s_out.h"
#include "Driver/i2s_fractional_timing.h"
#include "Driver/StepTimer.h"
#include "hal/i2s_hal.h"

#include <sdkconfig.h>

#include "Driver/delay_usecs.h"  // delay_us()

#include <esp_attr.h>  // IRAM_ATTR

#include <freertos/FreeRTOS.h>

#include <driver/periph_ctrl.h>
#include <rom/lldesc.h>
#include <soc/i2s_struct.h>
#include <soc/gpio_periph.h>
#include "Driver/fluidnc_gpio.h"

#include "esp_intr_alloc.h"
#include <xtensa/core-macros.h>

uint32_t i2s_frame_us;  // 1, 2 or 4

static volatile uint32_t i2s_out_port_data = 0;

static bool i2s_out_initialized = 0;

static pinnum_t i2s_out_ws_pin   = INVALID_PINNUM;
static pinnum_t i2s_out_bck_pin  = INVALID_PINNUM;
static pinnum_t i2s_out_data_pin = INVALID_PINNUM;

static inline void i2s_out_reset_tx_rx() {
    i2s_ll_tx_reset(&I2S0);
    i2s_ll_rx_reset(&I2S0);
}

static inline void i2s_out_reset_fifo_without_lock() {
    i2s_ll_tx_reset_fifo(&I2S0);
    i2s_ll_rx_reset_fifo(&I2S0);
}

static void i2s_out_gpio_attach(pinnum_t ws, pinnum_t bck, pinnum_t data) {
    // Route the i2s pins to the appropriate GPIO
    gpio_route(data, I2S0O_DATA_OUT23_IDX);
    gpio_route(bck, I2S0O_BCK_OUT_IDX);
    gpio_route(ws, I2S0O_WS_OUT_IDX);
}

const int I2S_OUT_DETACH_PORT_IDX = 0x100;

static void i2s_out_gpio_detach(pinnum_t ws, pinnum_t bck, pinnum_t data) {
    // Route the i2s pins to the appropriate GPIO
    gpio_route(ws, I2S_OUT_DETACH_PORT_IDX);
    gpio_route(bck, I2S_OUT_DETACH_PORT_IDX);
    gpio_route(data, I2S_OUT_DETACH_PORT_IDX);
}

static void i2s_out_gpio_shiftout(uint32_t port_data) {
    gpio_write(i2s_out_ws_pin, 0);
    for (size_t i = 0; i < I2S_OUT_NUM_BITS; i++) {
        gpio_write(i2s_out_data_pin, !!(port_data & (1 << (I2S_OUT_NUM_BITS - 1 - i))));
        gpio_write(i2s_out_bck_pin, 1);
        gpio_write(i2s_out_bck_pin, 0);
    }
    gpio_write(i2s_out_ws_pin, 1);  // Latch
}

static void i2s_out_stop() {
    // stop TX module
    i2s_ll_tx_stop(&I2S0);

    // Force WS to LOW before detach
    // This operation prevents unintended WS edge trigger when detach
    gpio_write(i2s_out_ws_pin, 0);

    // Now, detach GPIO pin from I2S
    i2s_out_gpio_detach(i2s_out_ws_pin, i2s_out_bck_pin, i2s_out_data_pin);

    // Force BCK to LOW
    // After the TX module is stopped, BCK always seems to be in LOW.
    // However, I'm going to do it manually to ensure the BCK's LOW.
    gpio_write(i2s_out_bck_pin, 0);

    // Transmit recovery data to 74HC595
    uint32_t port_data = i2s_out_port_data;  // current expanded port value
    i2s_out_gpio_shiftout(port_data);
}

static void i2s_out_start() {
    if (!i2s_out_initialized) {
        return;
    }

    // Transmit recovery data to 74HC595
    uint32_t port_data = i2s_out_port_data;  // current expanded port value
    i2s_out_gpio_shiftout(port_data);

    // Attach I2S to specified GPIO pin
    i2s_out_gpio_attach(i2s_out_ws_pin, i2s_out_bck_pin, i2s_out_data_pin);

    // reset TX/RX module
    // reset FIFO
    i2s_out_reset_tx_rx();
    i2s_out_reset_fifo_without_lock();

    // i2s_ll_tx_set_chan_mod(&I2S0, I2S_CHANNEL_FMT_RIGHT_LEFT);
    i2s_ll_tx_stop_on_fifo_empty(&I2S0, true);
    i2s_ll_tx_start(&I2S0);

    // Wait for the first FIFO data to prevent the unintentional generation of 0 data
    delay_us(20);
    i2s_ll_tx_stop_on_fifo_empty(&I2S0, false);
}

// The key FIFO parameters are FIFO_THRESHOLD and FIFO_RELOAD
// Their sum must be less than FIFO_LENGTH (64 for ESP32).
// - FIFO_THRESHOLD is the level at which the interrupt fires.
// If it is too low, you risk FIFO underflow.  Higher values
// allow more leeway for interrupt latency, but increase the
// latency between the software step generation and the appearance
// of step pulses at the driver.
// - FIFO_RELOAD is the number of entries that each ISR invocation
// pushes into the FIFO.  Larger values of FIFO_RELOAD decrease
// the number of times that the ISR runs, while smaller values
// decrease the step generation latency.
// - With an I2S frame clock of 500 kHz, FIFO_THRESHOLD = 16,
// FIFO_RELOAD = 8, the step latency is about 24 us.  That
// is about half of the modulation period of a laser that
// is modulated at 20 kHZ.

#define FIFO_LENGTH (I2S_TX_DATA_NUM + 1)
static uint32_t _fifo_threshold = FIFO_LENGTH / 4;
static uint32_t _fifo_reload    = 8;

static bool timer_running = false;

void i2s_out_delay() {
    // Empirically, FIFO_LENGTH/2 seems to be enough, but we use
    // FIFO_LENGTH to be safe.  This function is used infrequently,
    // typically only when setting up TMC drivers, so the extra
    // delay does not affect the performance significantly.
    uint32_t wait_counts = FIFO_LENGTH;
    delay_us(i2s_frame_us * wait_counts);
}

void IRAM_ATTR i2s_out_write(pinnum_t pin, uint8_t val) {
    uint32_t bit = 1 << pin;
    if (val) {
        i2s_out_port_data |= bit;
    } else {
        i2s_out_port_data &= ~bit;
    }

    if (!timer_running) {
        // Direct write to the I2S FIFO in case the pulse timer is not running
        I2S0.fifo_wr = i2s_out_port_data;
    }
}

uint8_t i2s_out_read(pinnum_t pin) {
    uint32_t port_data = i2s_out_port_data;
    return !!(port_data & (1 << pin));
}

void i2s_out_init(i2s_out_init_t* init_param) {
    if (i2s_out_initialized) {
        return;
    }

    i2s_frame_us = init_param->min_pulse_us;
    _fifo_threshold = init_param->fifo_threshold ? init_param->fifo_threshold : FIFO_LENGTH / 4;
    _fifo_reload    = init_param->fifo_reload ? init_param->fifo_reload : 8;

    i2s_out_port_data = init_param->init_val;

    // To make sure hardware is enabled before any hardware register operations.
    periph_module_reset(PERIPH_I2S0_MODULE);
    periph_module_enable(PERIPH_I2S0_MODULE);

    // Route the i2s pins to the appropriate GPIO
    i2s_out_gpio_attach(init_param->ws_pin, init_param->bck_pin, init_param->data_pin);
    if (init_param->ws_drive_strength != -1) {
        gpio_drive_strength(init_param->ws_pin, init_param->ws_drive_strength);
    }
    if (init_param->ws_drive_strength != -1) {
        gpio_drive_strength(init_param->ws_pin, init_param->ws_drive_strength);
    }
    if (init_param->bck_drive_strength != -1) {
        gpio_drive_strength(init_param->bck_pin, init_param->bck_drive_strength);
    }
    if (init_param->data_drive_strength != -1) {
        gpio_drive_strength(init_param->data_pin, init_param->data_drive_strength);
    }

    /**
   * Each i2s transfer will take
   *   fpll = PLL_D2_CLK      -- clka_en = 0
   *
   *   fi2s = fpll / N + b/a  -- N + b/a = clkm_div_num
   *   fi2s = 160MHz / 2
   *   fi2s = 80MHz
   *
   *   fbclk = fi2s / M   -- M = tx_bck_div_num
   *   fbclk = 80MHz / 2
   *   fbclk = 40MHz
   *
   *   fwclk = fbclk / 32
   *
   *   for fwclk = 250kHz(16-bit: 4µS pulse time), 125kHz(32-bit: 8μS pulse time)
   *      N = 10, b/a = 0
   *      M = 2
   *   for fwclk = 500kHz(16-bit: 2µS pulse time), 250kHz(32-bit: 4μS pulse time)
   *      N = 5, b/a = 0
   *      M = 2
   *   for fwclk = 1000kHz(16-bit: 1µS pulse time), 500kHz(32-bit: 2μS pulse time)
   *      N = 2, b/a = 2/1 (N + b/a = 2.5)
   *      M = 2
   */

    // stop i2s
    i2s_ll_tx_stop_link(&I2S0);
    i2s_ll_tx_stop(&I2S0);

    // i2s_param_config

    // configure I2S data port interface.

    i2s_out_reset_fifo_without_lock();

    i2s_ll_enable_lcd(&I2S0, false);
    i2s_ll_enable_camera(&I2S0, false);
#ifdef SOC_I2S_SUPPORTS_PDM_TX
    i2s_ll_tx_enable_pdm(&I2S0, false);
#endif

    i2s_ll_enable_dma(&I2S0, false);

    i2s_ll_tx_set_chan_mod(&I2S0, I2S_CHANNEL_FMT_RIGHT_LEFT);  // Overridden by i2s_out_start

    i2s_ll_tx_set_sample_bit(&I2S0, I2S_BITS_PER_SAMPLE_32BIT, I2S_BITS_PER_SAMPLE_16BIT);
    i2s_ll_tx_enable_mono_mode(&I2S0, false);

    i2s_ll_enable_dma(&I2S0, false);  // FIFO is not connected to DMA
    i2s_ll_tx_stop(&I2S0);
    i2s_ll_rx_stop(&I2S0);

    i2s_ll_tx_enable_msb_right(&I2S0, true);  // Place right-channel data at the MSB in the transmit FIFO.

    i2s_ll_tx_enable_right_first(&I2S0, false);  // Send the left-channel data first
    // i2s_ll_tx_enable_right_first(&I2S0, true);  // Send the right-channel data first

    i2s_ll_tx_set_slave_mod(&I2S0, false);  // Master
    i2s_ll_tx_force_enable_fifo_mod(&I2S0, true);
#ifdef SOC_I2S_SUPPORTS_PDM_TX
    i2s_ll_tx_enable_pdm(&I2S0, false);
#endif

    // I2S_COMM_FORMAT_I2S_LSB
    i2s_ll_tx_set_ws_width(&I2S0, 0);          // PCM standard mode.
    i2s_ll_tx_enable_msb_shift(&I2S0, false);  // Do not use the Philips standard to avoid bit-shifting

#ifdef CONFIG_IDF_TARGET_ESP32
    i2s_ll_tx_clk_set_src(&I2S0, I2S_CLK_D2CLK);
#endif
    // N + b/a = 0
    //    i2s_ll_mclk_div_t first_div = { 2, 3, 47 };  // { N, b, a }
    //    i2s_ll_tx_set_clk(&I2S0, &first_div);

    i2s_ll_mclk_div_t div = { 5, 0, 0 };
    switch (i2s_frame_us) {
        case 1:
            div.mclk_div = 2;  // Fractional divisor 2.5, i.e. 2 + 16/32
            div.a        = 32;
            div.b        = 16;
            break;
        case 2:
            div.mclk_div = 5;
            break;

        case 4:
        default:
            div.mclk_div = 10;
            break;
    }
    i2s_ll_tx_set_clk(&I2S0, &div);

    i2s_ll_tx_set_bck_div_num(&I2S0, 2);

    // Remember GPIO pin numbers
    i2s_out_ws_pin      = init_param->ws_pin;
    i2s_out_bck_pin     = init_param->bck_pin;
    i2s_out_data_pin    = init_param->data_pin;
    i2s_out_initialized = 1;

    // Start the I2S peripheral
    i2s_out_start();
}

// Interface to step engine

static uint32_t _pulse_counts = 2;
static uint32_t _dir_delay_us;

bool (*_pulse_func)();

static uint32_t _remaining_pulse_counts = 0;
static uint32_t _remaining_delay_counts = 0;
static uint32_t _remaining_direction_counts = 0;
static volatile uint32_t _pending_direction_counts = 0;

static uint32_t _pulse_data;
static i2s_fractional_timing_t _planner_frame_timing;

// Continuous C uses the same planner callback and pulse word as X/Z.  The I2S
// layer only watches for transport underruns while that shared scheduler owns C.
static volatile bool _continuous_transport_active        = false;
static volatile bool _continuous_transport_fault_pending = false;
static volatile bool _continuous_transport_faulted       = false;

static volatile uint32_t _diag_underruns                 = 0;
static volatile uint32_t _diag_max_isr_gap_cycles        = 0;
static volatile uint32_t _diag_max_isr_duration_cycles = 0;
static volatile uint32_t _diag_last_isr_cycle            = 0;

// The I2S ISR is the only writer. Task context reads this snapshot only after
// the planner reaches Idle, so these diagnostic-only counters need no volatile
// barriers in the interval hot path.
typedef struct {
    uint32_t interval_ticks;
    uint32_t interval_frames;
    uint32_t fractional_residual_ticks;
    uint32_t scheduled_ticks;
    uint32_t emitted_frames;
    uint32_t emitted_intervals;
    bool     active;
} i2s_planner_diagnostics_state_t;

static i2s_planner_diagnostics_state_t _diag_planner = { 0 };

static void IRAM_ATTR set_timer_ticks(uint32_t ticks) {
    if (ticks) {
        i2s_fractional_timing_set_interval(&_planner_frame_timing, ticks);
        _diag_planner.interval_ticks = ticks;
    }
}

static uint32_t IRAM_ATTR next_planner_interval_frames(bool callback_active) {
    const uint32_t frames = i2s_fractional_timing_next(&_planner_frame_timing);
    if (callback_active) {
        if (!_diag_planner.active) {
            // Retain the completed run until the first active callback of the
            // next run, so task-context diagnostics can inspect it at Idle.
            _diag_planner.scheduled_ticks   = 0;
            _diag_planner.emitted_frames    = 0;
            _diag_planner.emitted_intervals = 0;
            _diag_planner.active            = true;
        }
        _diag_planner.interval_frames           = frames;
        _diag_planner.fractional_residual_ticks = _planner_frame_timing.residual_ticks;
        _diag_planner.scheduled_ticks += _planner_frame_timing.interval_ticks;
        _diag_planner.emitted_frames += frames;
        ++_diag_planner.emitted_intervals;
    } else {
        _diag_planner.active = false;
        // The final interval consumes the existing residual. Idle callbacks
        // start from zero, so no fractional state crosses planner motions.
        i2s_fractional_timing_reset(&_planner_frame_timing);
    }
    return frames;
}

static void IRAM_ATTR start_timer() {
    if (!timer_running) {
        i2s_ll_enable_intr(&I2S0, I2S_TX_PUT_DATA_INT_ENA, 1);
        i2s_ll_clear_intr_status(&I2S0, I2S_PUT_DATA_INT_CLR);
        timer_running = true;
    }
}
static void IRAM_ATTR stop_timer() {
    if (timer_running) {
        i2s_ll_enable_intr(&I2S0, I2S_TX_PUT_DATA_INT_ENA, 0);
        timer_running = false;
    }
    i2s_fractional_timing_reset(&_planner_frame_timing);
}

static void IRAM_ATTR i2s_isr() {
    const uint32_t isr_started = XTHAL_GET_CCOUNT();
    const uint32_t prior_isr   = _diag_last_isr_cycle;
    _diag_last_isr_cycle       = isr_started;
    if (prior_isr != 0) {
        const uint32_t gap = isr_started - prior_isr;
        if (gap > _diag_max_isr_gap_cycles) {
            _diag_max_isr_gap_cycles = gap;
        }
    }

    if (I2S0.int_raw.tx_rempty) {
        I2S0.int_clr.tx_rempty = 1;
        ++_diag_underruns;
        if (_continuous_transport_active) {
            // The shared scheduler must fail closed. Foreground service turns
            // this latch into a spindle-control alarm; stale timing is unsafe.
            _continuous_transport_active        = false;
            _continuous_transport_faulted       = true;
            _continuous_transport_fault_pending = true;
        }
    }

    // Keeping local copies of this information speeds up the ISR
    uint32_t pulse_data             = _pulse_data;
    uint32_t remaining_pulse_counts = _remaining_pulse_counts;
    uint32_t remaining_delay_counts = _remaining_delay_counts;
    uint32_t remaining_direction_counts = _remaining_direction_counts;

    int i = _fifo_reload;
    do {
        if (remaining_direction_counts) {
            I2S0.fifo_wr = i2s_out_port_data;
            --i;
            --remaining_direction_counts;
        } else if (remaining_pulse_counts) {
            I2S0.fifo_wr = pulse_data;
            --i;
            --remaining_pulse_counts;
        } else if (remaining_delay_counts) {
            I2S0.fifo_wr = i2s_out_port_data;
            --i;
            --remaining_delay_counts;
        } else {
            _pulse_data = i2s_out_port_data;
            const bool callback_active = _pulse_func();
            pulse_data             = _pulse_data;
            remaining_pulse_counts = pulse_data == i2s_out_port_data ? 0 : _pulse_counts;
            const uint32_t interval_frames = next_planner_interval_frames(callback_active);
            remaining_delay_counts = interval_frames > remaining_pulse_counts
                                         ? interval_frames - remaining_pulse_counts
                                         : 0;
            if (_pending_direction_counts) {
                remaining_direction_counts = _pending_direction_counts;
                _pending_direction_counts  = 0;
            }
        }
    } while (i);

    // Save the counts back to the variables
    _remaining_pulse_counts = remaining_pulse_counts;
    _remaining_delay_counts = remaining_delay_counts;
    _remaining_direction_counts = remaining_direction_counts;

    // Clear the interrupt after pushing new data into the FIFO.  If you clear
    // it before, the interrupt will re-fire back because the FIFO is still
    // below the threshold.
    i2s_ll_clear_intr_status(&I2S0, I2S_PUT_DATA_INT_CLR);
    const uint32_t duration = XTHAL_GET_CCOUNT() - isr_started;
    if (duration > _diag_max_isr_duration_cycles) {
        _diag_max_isr_duration_cycles = duration;
    }
}

static void i2s_fifo_intr_setup() {
    I2S0.fifo_conf.tx_data_num = _fifo_threshold;
    esp_intr_alloc_intrstatus(ETS_I2S0_INTR_SOURCE,
                              ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL3,
                              (uint32_t)i2s_ll_get_intr_status_reg(&I2S0),
                              I2S_PUT_DATA_INT_CLR_M,
                              i2s_isr,
                              NULL,
                              NULL);
}

static uint32_t init_engine(uint32_t dir_delay_us, uint32_t pulse_us, uint32_t frequency, bool (*callback)(void)) {
    _pulse_func = callback;
    i2s_fifo_intr_setup();

    if (pulse_us < i2s_frame_us) {
        pulse_us = i2s_frame_us;
    }
    if (pulse_us > I2S_MAX_USEC_PER_PULSE) {
        pulse_us = I2S_MAX_USEC_PER_PULSE;
    }
    _dir_delay_us = dir_delay_us;
    _pulse_counts = (pulse_us + i2s_frame_us - 1) / i2s_frame_us;
    i2s_fractional_timing_init(&_planner_frame_timing, frequency * i2s_frame_us / 1000000);

    _remaining_pulse_counts = 0;
    _remaining_delay_counts = 0;
    _remaining_direction_counts = 0;
    _pending_direction_counts   = 0;

    // gpio_mode(12, 0, 1, 0, 0, 0);

    // Run the pulser all the time to pick up writes to non-stepping I2S outputs
    set_timer_ticks(100);
    start_timer();

    return _pulse_counts * i2s_frame_us;
}

static uint32_t init_step_pin(pinnum_t step_pin, bool step_invert) {
    return step_pin;
}

// This modifies a memory variable that contains the desired
// pin states.  Later, that variable will be transferred to
// the I2S FIFO to change all the affected pins at once.
static IRAM_ATTR void set_dir_pin(pinnum_t pin, bool level) {
    i2s_out_write(pin, level);
}

// Direction frames are queued through the same compositor as every planner
// frame. Direct FIFO writes here could bypass C pulses and overfill a deep FIFO.
static IRAM_ATTR void finish_dir() {
    uint32_t counts = (_dir_delay_us + i2s_frame_us - 1) / i2s_frame_us;
    _pending_direction_counts = counts ? counts : 1;
}

static void IRAM_ATTR start_step() {
    _pulse_data = i2s_out_port_data;
}

static IRAM_ATTR void set_step_pin(pinnum_t pin, bool level) {
    uint32_t bit = 1 << pin;
    if (level) {
        _pulse_data |= bit;
    } else {
        _pulse_data &= ~bit;
    }
}

static void IRAM_ATTR finish_step() {}

static bool IRAM_ATTR start_unstep() {
    return true;  // Skip the rest of the step process
}

// Not called since start_unstep() returns 1
static IRAM_ATTR void finish_unstep() {}

static uint32_t max_pulses_per_sec() {
    return 1000000 / (2 * _pulse_counts * i2s_frame_us);
}

// clang-format off
step_engine_t i2s_engine = {
    "I2S",
    init_engine,
    init_step_pin,
    set_dir_pin,
    finish_dir,
    start_step,
    set_step_pin,
    finish_step,
    start_unstep,
    finish_unstep,
    max_pulses_per_sec,
    set_timer_ticks,
    start_timer,
    stop_timer,
    NULL
};
// clang-format on
REGISTER_STEP_ENGINE(I2S, &i2s_engine);

bool i2s_out_continuous_transport_start() {
    if (!i2s_out_initialized || _continuous_transport_active) {
        return false;
    }
    _continuous_transport_fault_pending = false;
    _continuous_transport_faulted       = false;
    _diag_underruns                     = 0;
    _diag_max_isr_gap_cycles            = 0;
    _diag_max_isr_duration_cycles       = 0;
    _diag_last_isr_cycle                = 0;
    I2S0.int_clr.tx_rempty              = 1;
    _continuous_transport_active        = true;
    return true;
}

void i2s_out_continuous_transport_stop() {
    _continuous_transport_active = false;
}

bool IRAM_ATTR i2s_out_continuous_transport_active() {
    return _continuous_transport_active;
}

bool IRAM_ATTR i2s_out_continuous_transport_faulted() {
    return _continuous_transport_faulted;
}

bool i2s_out_continuous_transport_take_fault() {
    const bool pending = _continuous_transport_fault_pending;
    _continuous_transport_fault_pending = false;
    return pending;
}

void i2s_out_get_diagnostics(i2s_out_diagnostics_t* diagnostics) {
    if (diagnostics == NULL) {
        return;
    }
    diagnostics->fifo_threshold          = _fifo_threshold;
    diagnostics->fifo_reload             = _fifo_reload;
    diagnostics->underruns               = _diag_underruns;
    diagnostics->max_isr_gap_us          = _diag_max_isr_gap_cycles / CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ;
    diagnostics->max_isr_duration_us     = _diag_max_isr_duration_cycles / CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ;
    diagnostics->planner_interval_ticks  = _diag_planner.interval_ticks;
    diagnostics->planner_interval_frames = _diag_planner.interval_frames;
    diagnostics->planner_fractional_residual_ticks = _diag_planner.fractional_residual_ticks;
    diagnostics->planner_scheduled_ticks          = _diag_planner.scheduled_ticks;
    diagnostics->planner_emitted_frames           = _diag_planner.emitted_frames;
    diagnostics->planner_emitted_intervals        = _diag_planner.emitted_intervals;
    diagnostics->planner_active                   = _diag_planner.active;
    diagnostics->transport_faulted                = _continuous_transport_faulted;
}
