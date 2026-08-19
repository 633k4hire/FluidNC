#include "Driver/i2s_out.h"
void i2s_out_init(i2s_out_init_t* params) {
    return;
}
bool i2s_out_continuous_transport_start() { return false; }
void i2s_out_continuous_transport_stop() {}
bool i2s_out_continuous_transport_active() { return false; }
bool i2s_out_continuous_transport_faulted() { return false; }
bool i2s_out_continuous_transport_take_fault() { return false; }
void i2s_out_get_diagnostics(i2s_out_diagnostics_t* diagnostics) {
    if (diagnostics) {
        *diagnostics = {};
    }
}
uint32_t i2s_out_timeline_frames() { return 0; }
