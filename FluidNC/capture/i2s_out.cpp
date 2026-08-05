#include "Driver/i2s_out.h"
void i2s_out_init(i2s_out_init_t* params) {
    return;
}
bool i2s_out_aux_step_start(pinnum_t, bool, pinnum_t, bool, uint32_t, uint32_t, i2s_out_aux_pulse_callback_t) { return false; }
void i2s_out_aux_step_set_rate(uint32_t) {}
void i2s_out_aux_step_stop(bool) {}
bool i2s_out_aux_step_active() { return false; }
uint32_t i2s_out_aux_step_current_rate_millihz() { return 0; }
