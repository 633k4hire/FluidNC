// Copyright (c) 2026 FluidNC contributors
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "maijker_turret.h"

#include "NutsBolts.h"
#include "Protocol.h"
#include "State.h"
#include "System.h"

#include <stdexcept>

namespace ATCs {
    namespace {
        MaijkerTurret* active_turret = nullptr;

        bool valid_tool_or_zero(uint32_t tool, uint32_t station_count) {
            return tool == 0 || MaijkerTurretLogic::valid_station_tool(tool, station_count);
        }
    }

    MaijkerTurret::MaijkerTurret(const char* name) : ATC(name), _sensor_pin("sensor_pin") {
        if (active_turret == nullptr) {
            active_turret = this;
        }
    }

    void MaijkerTurret::group(Configuration::HandlerBase& handler) {
        handler.item("station_count", _station_count, 2, 32);
        handler.item("step_pin", _step_pin);
        handler.item("direction_pin", _direction_pin);
        handler.item("forward_direction", _forward_direction);
        handler.item("steps_per_station", _steps_per_station, 1, 1000000);
        handler.item("step_rate_hz", _step_rate_hz, 1, 100000);
        handler.item("pulse_us", _pulse_us, 1, 1000000);
        handler.item("dir_setup_ms", _dir_setup_ms, 0, 60000);
        handler.item("overshoot_steps", _overshoot_steps, 0, 1000000);
        handler.item("lock_backoff_steps", _lock_backoff_steps, 0, 1000000);
        handler.item("lock_settle_ms", _lock_settle_ms, 0, 60000);
        handler.item("require_confirmed_tool", _require_confirmed_tool);
        handler.item("current_tool", _current_tool, 0, 32);
        handler.item("sensor_pin", _sensor_pin);
        handler.item("sensor_tool", _sensor_tool, 1, 32);
        handler.item("sensor_debounce_ms", _sensor_debounce_ms, 0, 60000);
        handler.item("home_timeout_steps", _home_timeout_steps, 1, 10000000);
    }

    void MaijkerTurret::validate() {
        if (!_step_pin.defined()) {
            throw std::runtime_error("Maijker turret requires step_pin");
        }
        if (!_direction_pin.defined()) {
            throw std::runtime_error("Maijker turret requires direction_pin");
        }
        if (!MaijkerTurretLogic::valid_config(logic_config())) {
            throw std::runtime_error("Maijker turret station_count and steps_per_station must be valid");
        }
        if (!valid_tool_or_zero(_current_tool, _station_count)) {
            throw std::runtime_error("Maijker turret current_tool must be 0 or a valid station");
        }
        if (!MaijkerTurretLogic::valid_station_tool(_sensor_tool, _station_count)) {
            throw std::runtime_error("Maijker turret sensor_tool must be a valid station");
        }
        const uint32_t period_us = 1000000UL / _step_rate_hz;
        if (period_us <= _pulse_us) {
            throw std::runtime_error("Maijker turret pulse_us must be shorter than the step period");
        }
    }

    void MaijkerTurret::init() {
        _step_pin.setAttr(Pin::Attr::Output);
        _direction_pin.setAttr(Pin::Attr::Output);
        _step_pin.synchronousWrite(false);
        _direction_pin.write(_forward_direction);

        if (sensor_configured()) {
            _sensor_pin.init();
        }

        _tool_confirmed = MaijkerTurretLogic::valid_station_tool(_current_tool, _station_count);
        _initialized    = true;
        _last_error     = _tool_confirmed ? "ok" : "current tool is not confirmed";

        log_info("ATC:" << name() << " stations:" << _station_count << " step:" << _step_pin.name() << " dir:" << _direction_pin.name()
                        << " confirmed_tool:" << _current_tool);
    }

    void MaijkerTurret::probe_notification() {}

    bool MaijkerTurret::tool_change(tool_t value, bool pre_select, bool set_tool) {
        const uint32_t requested_tool = value;
        if (pre_select) {
            return true;
        }

        if (!_initialized) {
            set_error("turret is not initialized");
            return false;
        }

        if (set_tool) {
            return configure_tool_state(requested_tool);
        }

        protocol_buffer_synchronize();
        if (check_abort_or_alarm()) {
            return false;
        }

        _target_tool = requested_tool;
        const auto plan = MaijkerTurretLogic::build_change_plan(logic_config(), _current_tool, _tool_confirmed, requested_tool);
        if (plan.error != MaijkerTurretLogic::Error::None) {
            set_error(plan.error);
            _target_tool = 0;
            return false;
        }
        if (plan.delta_stations == 0) {
            _last_error  = "ok";
            _target_tool = 0;
            return true;
        }

        if (!run_change(plan)) {
            _target_tool = 0;
            return false;
        }

        _current_tool    = requested_tool;
        _tool_confirmed = true;
        _target_tool    = 0;
        _last_error     = "ok";
        return true;
    }

    Error MaijkerTurret::home() {
        if (!_initialized) {
            set_error("turret is not initialized");
            return Error::InvalidValue;
        }
        if (!sensor_configured()) {
            set_error(MaijkerTurretLogic::Error::SensorUnavailable);
            return Error::InvalidValue;
        }

        protocol_buffer_synchronize();
        if (check_abort_or_alarm()) {
            return Error::Reset;
        }
        if (!set_direction(true)) {
            return Error::Reset;
        }

        bool found = sensor_active_debounced();
        for (uint32_t steps = 0; !found && steps < _home_timeout_steps; ++steps) {
            if (!pulse_steps(1)) {
                return Error::Reset;
            }
            found = sensor_active_debounced();
        }

        const auto result = MaijkerTurretLogic::build_home_result(true, found, _sensor_tool, _station_count);
        if (result.error != MaijkerTurretLogic::Error::None) {
            set_error(result.error);
            return Error::InvalidValue;
        }

        _current_tool    = result.tool_number;
        _tool_confirmed = result.confirmed;
        _target_tool    = 0;
        _last_error     = "ok";
        return Error::Ok;
    }

    MaijkerTurretStatus MaijkerTurret::status() const {
        MaijkerTurretStatus status;
        status.configured        = true;
        status.current_tool      = _current_tool;
        status.target_tool       = _target_tool;
        status.tool_confirmed    = _tool_confirmed;
        status.sensor_configured = sensor_configured();
        status.sensor_active     = sensor_active();
        status.last_error        = _last_error.c_str();
        return status;
    }

    MaijkerTurretLogic::Config MaijkerTurret::logic_config() const {
        MaijkerTurretLogic::Config config;
        config.station_count          = _station_count;
        config.steps_per_station      = _steps_per_station;
        config.overshoot_steps        = _overshoot_steps;
        config.lock_backoff_steps     = _lock_backoff_steps;
        config.require_confirmed_tool = _require_confirmed_tool;
        return config;
    }

    bool MaijkerTurret::configure_tool_state(uint32_t tool) {
        if (tool == 0) {
            _current_tool    = 0;
            _target_tool     = 0;
            _tool_confirmed = false;
            _last_error     = "current tool is not confirmed";
            return true;
        }
        if (!MaijkerTurretLogic::valid_station_tool(tool, _station_count)) {
            set_error(MaijkerTurretLogic::Error::InvalidTargetTool);
            return false;
        }
        _current_tool    = tool;
        _target_tool     = 0;
        _tool_confirmed = true;
        _last_error     = "ok";
        return true;
    }

    bool MaijkerTurret::run_change(const MaijkerTurretLogic::ChangePlan& plan) {
        if (!set_direction(true)) {
            return false;
        }
        if (!pulse_steps(plan.forward_steps)) {
            return false;
        }
        if (!set_direction(false)) {
            return false;
        }
        if (!pulse_steps(plan.lock_steps)) {
            return false;
        }
        if (_lock_settle_ms && !dwell_ms(_lock_settle_ms)) {
            set_error("turret lock settle interrupted");
            return false;
        }
        _step_pin.synchronousWrite(false);
        return true;
    }

    bool MaijkerTurret::set_direction(bool forward) {
        const bool value = forward ? _forward_direction : !_forward_direction;
        _direction_pin.write(value);
        if (_dir_setup_ms && !dwell_ms(_dir_setup_ms)) {
            set_error("turret direction setup interrupted");
            return false;
        }
        return !check_abort_or_alarm();
    }

    bool MaijkerTurret::pulse_steps(uint32_t count) {
        const uint32_t period_us = 1000000UL / _step_rate_hz;
        const uint32_t low_us    = period_us > _pulse_us ? period_us - _pulse_us : 1;

        for (uint32_t step = 0; step < count; ++step) {
            protocol_execute_realtime();
            if (check_abort_or_alarm()) {
                _step_pin.synchronousWrite(false);
                return false;
            }
            _step_pin.synchronousWrite(true);
            delay_us(_pulse_us);
            _step_pin.synchronousWrite(false);
            delay_us(low_us);
        }
        return true;
    }

    bool MaijkerTurret::check_abort_or_alarm() {
        if (sys.abort()) {
            set_error("turret command aborted");
            return true;
        }
        if (state_is(State::Alarm) || state_is(State::Critical)) {
            set_error("alarm during turret command");
            return true;
        }
        return false;
    }

    bool MaijkerTurret::sensor_configured() const {
        return _sensor_pin.defined();
    }

    bool MaijkerTurret::sensor_active() const {
        return sensor_configured() && _sensor_pin.read();
    }

    bool MaijkerTurret::sensor_active_debounced() {
        if (!sensor_active()) {
            return false;
        }
        if (_sensor_debounce_ms && !dwell_ms(_sensor_debounce_ms)) {
            set_error("turret sensor debounce interrupted");
            return false;
        }
        return sensor_active();
    }

    void MaijkerTurret::set_error(const char* message) {
        _last_error = message;
        log_error("Maijker turret: " << message);
    }

    void MaijkerTurret::set_error(MaijkerTurretLogic::Error error) {
        set_error(MaijkerTurretLogic::error_message(error));
    }

    MaijkerTurretStatus maijker_turret_status() {
        if (active_turret == nullptr) {
            return {};
        }
        return active_turret->status();
    }

    Error maijker_turret_home() {
        if (active_turret == nullptr) {
            return Error::InvalidValue;
        }
        return active_turret->home();
    }

    namespace {
        ATCFactory::InstanceBuilder<MaijkerTurret> registration("maijker_5_station_turret");
    }
}
