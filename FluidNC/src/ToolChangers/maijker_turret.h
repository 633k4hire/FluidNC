// Copyright (c) 2026 FluidNC contributors
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

#include "Config.h"
#include "Error.h"
#include "Machine/InputPin.h"
#include "Pin.h"
#include "atc.h"
#include "maijker_turret_logic.h"

#include <cstdint>
#include <string>

namespace ATCs {
    struct MaijkerTurretStatus {
        bool        configured        = false;
        uint32_t    current_tool      = 0;
        uint32_t    target_tool       = 0;
        bool        tool_confirmed    = false;
        bool        sensor_configured = false;
        bool        sensor_active     = false;
        const char* last_error        = "not configured";
    };

    class MaijkerTurret : public ATC {
    public:
        MaijkerTurret(const char* name);

        void init() override;
        void probe_notification() override;
        bool tool_change(tool_t value, bool pre_select, bool set_tool) override;
        void validate() override;
        void group(Configuration::HandlerBase& handler) override;

        MaijkerTurretStatus status() const;
        Error home();

    private:
        uint32_t _station_count      = 5;
        uint32_t _steps_per_station  = 320;
        uint32_t _step_rate_hz       = 400;
        uint32_t _pulse_us           = 10;
        uint32_t _dir_setup_ms       = 10;
        uint32_t _overshoot_steps    = 32;
        uint32_t _lock_backoff_steps = 24;
        uint32_t _lock_settle_ms     = 250;
        uint32_t _current_tool       = 0;
        uint32_t _target_tool        = 0;
        uint32_t _sensor_tool        = 1;
        uint32_t _sensor_debounce_ms = 25;
        uint32_t _home_timeout_steps = 2000;
        bool     _forward_direction  = true;
        bool     _require_confirmed_tool = true;
        bool     _tool_confirmed          = false;
        bool     _initialized             = false;

        Pin      _step_pin;
        Pin      _direction_pin;
        InputPin _sensor_pin;

        std::string _last_error = "not initialized";

        MaijkerTurretLogic::Config logic_config() const;
        bool                       configure_tool_state(uint32_t tool);
        bool                       run_change(const MaijkerTurretLogic::ChangePlan& plan);
        bool                       set_direction(bool forward);
        bool                       pulse_steps(uint32_t count);
        bool                       check_abort_or_alarm();
        bool                       sensor_configured() const;
        bool                       sensor_active() const;
        bool                       sensor_active_debounced();
        void                       set_error(const char* message);
        void                       set_error(MaijkerTurretLogic::Error error);
    };

    MaijkerTurretStatus maijker_turret_status();
    Error               maijker_turret_home();
}
