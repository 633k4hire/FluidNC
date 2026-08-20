// Copyright (c) 2011-2016 Sungeun K. Jeon for Gnea Research LLC
// Copyright (c) 2009-2011 Simen Svale Skogsrud
// Copyright (c) 2018 -	Bart Dring
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

/*
  Stepper.h - stepper motor driver: executes motion plans of planner.c using the stepper motors
*/

#include "EnumItem.h"

#include <cstdint>

namespace Stepper {
    struct ThreadingExecution {
        uint32_t c_steps_per_revolution       = 0;
        uint32_t z_steps_per_revolution       = 0;
        uint32_t commanded_c_rate_millihz     = 0;
        uint32_t block_token                  = 0;
    };

    void init();

    bool pulse_func();

    // ISR-safe view of the finite block currently staged for execution. A
    // Phase 5 threading block is a straight Z-only move whose event cadence is
    // owned by the commanded continuous-C scheduler.
    bool threading_execution(ThreadingExecution& execution);

    // Enable steppers, but cycle does not start unless called by motion control or realtime command.
    void wake_up();

    // Stops stepping and disables stepper (not ISR-safe)
    void go_idle();
    bool is_awake();

    // Stops stepping (ISR-safe)
    void stop_stepping();

    // Reset the stepper subsystem variables
    void reset();
    // Flush finite planner motion while leaving a continuously-owned C spindle running.
    void resetPreservingContinuous();

    // Changes the run state of the step segment buffer to execute the special parking motion.
    void parking_setup_buffer();

    // Restores the step segment buffer to the normal run state after a parking motion.
    void parking_restore_buffer();

    // Reloads step segment buffer. Called continuously by realtime execution system.
    void prep_buffer();

    // Called by planner_recalculate() when the executing block is updated by the new plan.
    bool update_plan_block_parameters();

    // Called by realtime status reporting if realtime rate reporting is enabled in config.h.
    float get_realtime_rate();

    extern uint32_t isr_count;
}
