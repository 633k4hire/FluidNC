// Copyright (c) 2020 Mitch Bradley
// Copyright (c) 2014 Luc Lebosse. All rights reserved.
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

/*
  WebCommands.cpp - Settings and Commands for the interface
  to ESP3D_WebUI.  Code snippets extracted from commands.cpp in the
  old WebUI interface code are presented via the Settings class.
*/

#include "Settings.h"
#include "Authentication.h"
#include "Machine/MachineConfig.h"
#include "Configuration/JsonGenerator.h"
#include "Report.h"  // git_info
#include "GCode.h"
#include "Lathe.h"
#include "LatheEncoder.h"
#include "Alarm.h"
#include "Job.h"
#include "Limit.h"
#include "MotionControl.h"
#include "Planner.h"
#include "Stepper.h"
#include "Stepping.h"
#include "DialFirmwareClient.h"
#include "Machine/Homing.h"
#include "Spindles/Spindle.h"
#include "ToolChangers/maijker_turret.h"
#include "Driver/i2s_out.h"
#include "StaticMessageBufferPool.h"

#include <Esp.h>

#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <freertos/task.h>

#include "Module.h"

namespace WebUI {

    class WebCommands : public Module {
    public:
        WebCommands(const char* name) : Module(name) {}

        static std::string LeftJustify(const char* s, size_t width) {
            std::string ret(s);

            for (size_t l = ret.length(); width > l; width--) {
                ret += ' ';
            }
            return ret;
        }

        // Used by js/connectdlg.js

#ifdef ENABLE_AUTHENTICATION
        static Error setUserPassword(const char* parameter, AuthenticationLevel auth_level, Channel& out) {  // ESP555
            if (*parameter == '\0') {
                authentication_reset_user_password();
                return Error::Ok;
            }
            if (!authentication_set_password(false, parameter)) {
                log_string(out, "Invalid Password");
                return Error::InvalidValue;
            }
            return Error::Ok;
        }
#endif

        static Error restart(const char* parameter, AuthenticationLevel auth_level, Channel& out) {
            log_info("Restarting");
            protocol_send_event(&fullResetEvent);
            return Error::Ok;
        }

        // used by js/restartdlg.js
        static Error setSystemMode(const char* parameter, AuthenticationLevel auth_level, Channel& out) {  // ESP444
            // parameter = trim(parameter);
            if (strcasecmp(parameter, "RESTART") != 0) {
                log_string(out, "Parameter must be RESTART");
                return Error::InvalidValue;
            }
            return restart(parameter, auth_level, out);
        }

        // Used by js/statusdlg.js
        static Error showSysStatsJSON(const char* parameter, AuthenticationLevel auth_level, Channel& out) {  // ESP420

            JSONencoder j(&out);
            j.begin();
            j.member("cmd", "420");
            j.member("status", "ok");
            j.begin_array("data");

            j.id_value_object("Chip ID", (uint16_t)(ESP.getEfuseMac() >> 32));
            j.id_value_object("CPU Cores", ESP.getChipCores());

            std::ostringstream msg;
            msg << ESP.getCpuFreqMHz() << "Mhz";
            j.id_value_object("CPU Frequency", msg.str());

            std::ostringstream msg2;
            msg2 << std::fixed << std::setprecision(1) << temperatureRead() << "°C";
            j.id_value_object("CPU Temperature", msg2.str());

            j.id_value_object("Free memory", formatBytes(ESP.getFreeHeap()));
            j.id_value_object("SDK", ESP.getSdkVersion());
            j.id_value_object("Flash Size", formatBytes(ESP.getFlashChipSize()));

            for (auto const& module : ModuleFactory::objects()) {
                module->wifi_stats(j);
            }

            std::string s("FluidNC ");
            s += git_info;
            j.id_value_object("FW version", s);

            j.end_array();
            j.end();
            return Error::Ok;
        }

        static void send_json_command_response(Channel& out, uint cmdID, bool isok, const std::string& message) {
            JSONencoder j(&out);
            j.begin();
            j.member("cmd", String(cmdID).c_str());
            j.member("status", isok ? "ok" : "error");
            j.member("data", message);
            j.end();
        }


        static std::string float_string(float value, int precision = 3) {
            std::ostringstream msg;
            msg << std::fixed << std::setprecision(precision) << value;
            return msg.str();
        }

        static const char* lathe_spindle_mode_name() {
            return gc_state.modal.lathe_spindle_speed_mode == Lathe::SpindleSpeedMode::ConstantSurfaceSpeed ? "G96 CSS" : "G97 fixed RPM";
        }

        static const char* lathe_diameter_mode_name() {
            return gc_state.modal.lathe_diameter_mode == Lathe::DiameterMode::Diameter ? "G7 diameter" : "G8 radius";
        }

        static const char* feed_mode_name() {
            switch (gc_state.modal.feed_rate) {
                case FeedRate::InverseTime:
                    return "G93 inverse time";
                case FeedRate::UnitsPerRev:
                    return "G95 units/rev";
                case FeedRate::UnitsPerMin:
                    return "G94 units/min";
            }
            return "unknown";
        }

        static void json_literal(JSONencoder& j, const char* key, const std::string& value) {
            j.begin_member(key);
            j.verbatim(value);
        }

        static void json_bool(JSONencoder& j, const char* key, bool value) {
            json_literal(j, key, value ? "true" : "false");
        }

        static void json_number(JSONencoder& j, const char* key, uint64_t value) {
            json_literal(j, key, std::to_string(value));
        }

        static void json_number(JSONencoder& j, const char* key, int64_t value) {
            json_literal(j, key, std::to_string(value));
        }

        static void json_number(JSONencoder& j, const char* key, float value, int precision = 6) {
            if (!std::isfinite(value)) {
                json_literal(j, key, "null");
                return;
            }
            json_literal(j, key, float_string(value, precision));
        }

        static void json_nullable_number(JSONencoder& j, const char* key, bool available, float value, int precision = 6) {
            if (!available || !std::isfinite(value)) {
                json_literal(j, key, "null");
                return;
            }
            json_number(j, key, value, precision);
        }

        static const char* spindle_state_name(SpindleState state) {
            switch (state) {
                case SpindleState::Cw:
                    return "CLOCKWISE";
                case SpindleState::Ccw:
                    return "COUNTERCLOCKWISE";
                case SpindleState::Disable:
                    return "STOPPED";
                case SpindleState::Unknown:
                    return "UNKNOWN";
            }
            return "UNKNOWN";
        }

        static const char* motion_mode_name(Motion motion) {
            switch (motion) {
                case Motion::Seek:
                    return "G0_RAPID";
                case Motion::Linear:
                    return "G1_LINEAR";
                case Motion::CwArc:
                    return "G2_CLOCKWISE_ARC";
                case Motion::CcwArc:
                    return "G3_COUNTERCLOCKWISE_ARC";
                case Motion::Threading:
                    return "G33_THREADING";
                case Motion::LatheFinishingCycle:
                    return "G70_FINISHING";
                case Motion::LatheRoughingCycle:
                    return "G71_ROUGHING";
                case Motion::LatheGroovingCycle:
                    return "G75_GROOVING";
                case Motion::LatheThreadingCycle:
                    return "G76_THREADING";
                case Motion::LathePeckCycle:
                    return "G83_PECK";
                case Motion::ProbeToward:
                    return "G38_2_PROBE_TOWARD";
                case Motion::ProbeTowardNoError:
                    return "G38_3_PROBE_TOWARD_NO_ERROR";
                case Motion::ProbeAway:
                    return "G38_4_PROBE_AWAY";
                case Motion::ProbeAwayNoError:
                    return "G38_5_PROBE_AWAY_NO_ERROR";
                case Motion::None:
                    return "G80_NONE";
            }
            return "UNKNOWN";
        }

        static const char* telemetry_feed_mode_name(FeedRate mode) {
            switch (mode) {
                case FeedRate::InverseTime:
                    return "INVERSE_TIME";
                case FeedRate::UnitsPerRev:
                    return "UNITS_PER_REVOLUTION";
                case FeedRate::UnitsPerMin:
                    return "UNITS_PER_MINUTE";
            }
            return "UNKNOWN";
        }

        static const char* execution_name(State state) {
            switch (state) {
                case State::Cycle:
                case State::Jog:
                case State::Homing:
                    return "ACTIVE";
                case State::Hold:
                case State::Held:
                case State::SafetyDoor:
                    return "INTERRUPTED";
                case State::Idle:
                case State::CheckMode:
                    return "READY";
                case State::Alarm:
                case State::Critical:
                case State::ConfigAlarm:
                case State::Sleep:
                    return "STOPPED";
                case State::Starting:
                    return "UNAVAILABLE";
            }
            return "UNAVAILABLE";
        }

        static std::string coordinate_system_name(CoordIndex coordinate_system) {
            if (coordinate_system >= CoordIndex::G54 && coordinate_system <= CoordIndex::G59) {
                return "G" + std::to_string(54 + static_cast<int>(coordinate_system) - static_cast<int>(CoordIndex::G54));
            }
            if (coordinate_system >= CoordIndex::G59_1 && coordinate_system <= CoordIndex::G59_3) {
                return "G59." + std::to_string(1 + static_cast<int>(coordinate_system) - static_cast<int>(CoordIndex::G59_1));
            }
            return "UNKNOWN";
        }

        static bool control_pin_state(const char* legend, bool& configured, bool& active) {
            configured = false;
            active     = false;
            if (config == nullptr || config->_control == nullptr) {
                return false;
            }
            for (auto pin : config->_control->_pins) {
                if (strcmp(pin->legend(), legend) == 0) {
                    configured = pin->defined();
                    active     = configured && pin->get();
                    return true;
                }
            }
            return false;
        }

        static void json_axis(
            JSONencoder& j, const char* key, axis_t axis, const float* machine, const float* wco, MotorMask limits, AxisMask unhomed) {
            j.begin_member_object(key);
            json_bool(j, "available", axis < Axes::_numberAxis);
            j.member("native_units", axis == C_AXIS ? "DEGREE" : "MILLIMETER");
            json_number(j, "machine", machine[axis]);
            json_number(j, "work", machine[axis] - wco[axis]);
            json_number(j, "work_offset", wco[axis]);
            json_bool(j, "homed", bitnum_is_false(unhomed, axis));
            const bool limit_active = bitnum_is_true(limits, Machine::Axes::motor_bit(axis, 0)) ||
                                      bitnum_is_true(limits, Machine::Axes::motor_bit(axis, 1));
            json_bool(j, "limit_active", limit_active);
            j.end_object();
        }

        static void json_condition(JSONencoder& j, const char* level, const char* source, const char* code, const char* text) {
            j.begin_object();
            j.member("level", level);
            j.member("source", source);
            j.member("code", code);
            j.member("text", text);
            j.end_object();
        }

        static uint64_t monotonic_uptime_ms() {
            static uint32_t previous = 0;
            static uint64_t epoch    = 0;
            const uint32_t now       = millis();
            if (now < previous) {
                epoch += (uint64_t(1) << 32);
            }
            previous = now;
            return epoch + now;
        }

        static Error showTamsTelemetryJSON(const char* parameter, AuthenticationLevel auth_level, Channel& out) {  // ESP425
            constexpr size_t MaxTelemetryBytes = 8192;
            using TelemetryBufferPool = StaticMessageBufferPool<MaxTelemetryBytes + 1, 1>;
            static uint64_t sequence = 0;
            static TelemetryBufferPool telemetry_buffers;
            static std::mutex telemetry_mutex;

            // ESP425 can be requested by a physical serial client and the web
            // task at the same time. Each fixed buffer remains leased until
            // the destination has finished consuming it, so the next request
            // cannot overwrite a payload still queued to the UART output task.
            auto telemetry_buffer = telemetry_buffers.tryAcquire();
            while (!telemetry_buffer) {
                vTaskDelay(1);
                telemetry_buffer = telemetry_buffers.tryAcquire();
            }
            std::lock_guard<std::mutex> telemetry_guard(telemetry_mutex);
            size_t payload_size = 0;
            bool payload_overflow = false;

            float machine_position[MAX_N_AXIS] = {};
            float work_offset[MAX_N_AXIS]      = {};
            float last_probe_position[MAX_N_AXIS] = {};
            copyAxes(machine_position, get_mpos());
            copyAxes(work_offset, get_wco());
            steps_to_mpos(last_probe_position, probe_steps);

            int32_t  executing_gcode_line = gc_state.line_number;
            uint32_t executing_source_line = 0;
            bool     planner_line          = false;
            if (const auto block = plan_get_current_block()) {
                executing_gcode_line  = block->line_number;
                executing_source_line = block->source_line;
                planner_line          = true;
            }

            const bool program_active = Job::active() || (planner_line && executing_source_line > 0);
            const auto feedback       = spindle->latheFeedback().status();
            const auto spindle_state  = spindle->get_state();
            const auto turret         = ATCs::maijker_turret_status();
            const auto active_tool    = Lathe::active_tool_offset();
            const auto active_tool_data =
                active_tool.valid ? Lathe::get_tool_data(active_tool.tool_number) : std::optional<Lathe::ToolData> {};
            const auto coolant        = config->_coolant->get_state();
            const auto limit_state    = limits_get_state();
            const auto unhomed_axes   = Machine::Homing::unhomed_axes();
#ifdef TAMS_MAIJKER_ALARM_ASSETS
            AlarmTelemetryRecord alarm_history[AlarmTelemetryCapacity] = {};
            const size_t alarm_history_count =
                copy_alarm_telemetry(alarm_history, AlarmTelemetryCapacity);
#endif

            bool estop_configured = false;
            bool estop_active     = false;
            control_pin_state("estop_pin", estop_configured, estop_active);

            JSONencoder j([&](const char* fragment) {
                for (const char* cursor = fragment; *cursor != '\0'; ++cursor) {
                    if (*cursor == '\r' || *cursor == '\n') {
                        continue;
                    }
                    if (payload_size < MaxTelemetryBytes) {
                        telemetry_buffer.data()[payload_size++] = *cursor;
                    } else {
                        payload_overflow = true;
                    }
                }
            });
            j.begin();
            j.member("schema", "tams.fluidnc.telemetry.v1");
            json_number(j, "sequence", ++sequence);
            json_number(j, "uptime_ms", monotonic_uptime_ms());

            j.begin_member_object("machine");
            j.member("manufacturer", "TAMS");
            j.member("model", "Maijker XZACT Mini Lathe");
            j.member("configured_name", config->_name);
            j.member("board", config->_board);
            j.member("firmware", git_info);
            j.member("state", state_name());
            j.member("availability", sys.state() == State::Starting || sys.state() == State::ConfigAlarm ? "UNAVAILABLE" : "AVAILABLE");
            json_number(j, "alarm_code", static_cast<uint64_t>(lastAlarm));
            const char* alarm_name = alarmString(lastAlarm);
            j.member("alarm", alarm_name == nullptr ? "UNKNOWN" : alarm_name);
#ifdef TAMS_MAIJKER_ALARM_ASSETS
            const bool alarm_state =
                sys.state() == State::Alarm ||
                sys.state() == State::Critical ||
                sys.state() == State::ConfigAlarm;
            json_bool(j, "alarm_active", alarm_state && lastAlarm != ExecAlarm::None);
            j.member("alarm_native_code", alarm_native_code(lastAlarm));
            j.member("alarm_source", alarm_source(lastAlarm));
            j.member("alarm_native_severity", alarm_native_severity(lastAlarm));
#endif
            j.end_object();

#ifdef TAMS_MAIJKER_ALARM_ASSETS
            j.begin_array("alarm_history");
            for (size_t index = 0; index < alarm_history_count; ++index) {
                const auto& alarm_record = alarm_history[index];
                j.begin_object();
                json_number(j, "sequence", static_cast<uint64_t>(alarm_record.sequence));
                json_number(j, "occurred_uptime_ms", static_cast<uint64_t>(alarm_record.occurred_uptime_ms));
                json_number(j, "code", static_cast<uint64_t>(alarm_record.code));
                j.member("native_code", alarm_native_code(alarm_record.code));
                j.member("source", alarm_source(alarm_record.code));
                j.member("native_severity", alarm_native_severity(alarm_record.code));
                const char* history_text = alarmString(alarm_record.code);
                j.member("text", history_text == nullptr ? "Unknown controller alarm" : history_text);
                json_bool(
                    j,
                    "active",
                    alarm_state &&
                        alarm_record.code == lastAlarm &&
                        index + 1 == alarm_history_count);
                j.end_object();
            }
            j.end_array();
#endif

            j.begin_member_object("execution");
            j.member("state", execution_name(sys.state()));
            const char* controller_mode = program_active ? "AUTOMATIC"
                                          : (sys.state() == State::Jog || sys.state() == State::Homing) ? "MANUAL"
                                                                                                       : "MANUAL_DATA_INPUT";
            j.member("controller_mode", controller_mode);
            j.member("motion_mode", motion_mode_name(gc_state.modal.motion));
            j.member("feed_mode", telemetry_feed_mode_name(gc_state.modal.feed_rate));
            j.member("units", gc_state.modal.units == Units::Inches ? "INCH" : "MILLIMETER");
            j.member("distance_mode", gc_state.modal.distance == Distance::Absolute ? "ABSOLUTE" : "INCREMENTAL");
            switch (gc_state.modal.plane_select) {
                case Plane::XY:
                    j.member("plane", "XY");
                    break;
                case Plane::ZX:
                    j.member("plane", "ZX");
                    break;
                case Plane::YZ:
                    j.member("plane", "YZ");
                    break;
            }
            j.member("coordinate_system", coordinate_system_name(gc_state.modal.coord_select));
            json_number(j, "programmed_feed", gc_state.feed_rate);
            j.member(
                "programmed_feed_units",
                gc_state.modal.feed_rate == FeedRate::UnitsPerRev
                    ? (gc_state.modal.units == Units::Inches ? "INCH_PER_REVOLUTION" : "MILLIMETER_PER_REVOLUTION")
                    : (gc_state.modal.feed_rate == FeedRate::InverseTime
                           ? "PER_MINUTE"
                           : (gc_state.modal.units == Units::Inches ? "INCH_PER_MINUTE" : "MILLIMETER_PER_MINUTE")));
            json_number(j, "realtime_feed_mm_per_min", Stepper::get_realtime_rate());
            j.begin_member_object("program");
            j.member("name", Lathe::program_name());
            json_bool(j, "active", program_active);
            j.member("source", executing_source_line > 0 || Job::active() ? "FILE" : "STREAM");
            j.end_object();
            j.begin_member_object("line");
            if (executing_gcode_line > 0) {
                json_number(j, "gcode_n", static_cast<int64_t>(executing_gcode_line));
            } else {
                json_literal(j, "gcode_n", "null");
            }
            if (executing_source_line > 0) {
                json_number(j, "source", static_cast<uint64_t>(executing_source_line));
            } else {
                json_literal(j, "source", "null");
            }
            j.member("provenance", planner_line ? "PLANNER_EXECUTING" : "PARSER_LAST");
            j.end_object();
            j.end_object();

            j.begin_member_object("positions");
            json_axis(j, "x", X_AXIS, machine_position, work_offset, limit_state, unhomed_axes);
            json_axis(j, "z", Z_AXIS, machine_position, work_offset, limit_state, unhomed_axes);
            json_axis(j, "c", C_AXIS, machine_position, work_offset, limit_state, unhomed_axes);
            j.end_object();

            j.begin_member_object("spindle");
            json_bool(j, "shared_chuck", Lathe::shared_chuck_enabled());
            j.member("mode", Lathe::shared_chuck_mode_name(Lathe::shared_chuck_mode()));
            j.member("c_axis", "C");
            j.member("state", spindle_state_name(spindle_state));
            json_number(j, "programmed_s", gc_state.spindle_speed);
            json_number(j, "commanded_rpm", gc_state.lathe_commanded_rpm);
            json_number(j, "maximum_rpm", Lathe::max_css_rpm());
            json_nullable_number(j, "measured_rpm", feedback.has_measured_rpm, feedback.measured_rpm);
            j.member(
                "speed_mode",
                gc_state.modal.lathe_spindle_speed_mode == Lathe::SpindleSpeedMode::ConstantSurfaceSpeed ? "CONSTANT_SURFACE_SPEED"
                                                                                                         : "FIXED_RPM");
            j.member("diameter_mode", gc_state.modal.lathe_diameter_mode == Lathe::DiameterMode::Diameter ? "DIAMETER" : "RADIUS");
            j.begin_member_object("encoder");
            json_bool(j, "configured", Lathe::encoder_enabled());
            json_bool(j, "capture_active", Lathe::encoder_capture_active());
            json_number(j, "pulses_per_revolution", static_cast<uint64_t>(Lathe::encoder_pulses_per_revolution()));
            json_bool(j, "has_measured_rpm", feedback.has_measured_rpm);
            json_bool(j, "has_index", feedback.has_index_pulse);
            json_bool(j, "has_angular_position", feedback.has_angular_position);
            json_nullable_number(j, "angular_position_revolution", feedback.has_angular_position, feedback.angular_position_rev);
            json_number(j, "pulse_count", static_cast<uint64_t>(feedback.pulse_count));
            json_number(j, "index_count", static_cast<uint64_t>(feedback.index_count));
            json_number(j, "revolution_count", static_cast<uint64_t>(feedback.revolution_count));
            json_number(j, "last_index_pulses", static_cast<uint64_t>(feedback.last_index_pulses));
            json_number(j, "last_pulse_age_ms", static_cast<uint64_t>(feedback.last_pulse_age_ms));
            json_bool(j, "has_direction", feedback.has_direction);
            j.member("direction", feedback.has_direction ? (feedback.measured_direction > 0 ? "CW" : "CCW") : "UNKNOWN");
            json_bool(j, "stale", feedback.stale);
            json_bool(j, "fault", feedback.fault);
            j.end_object();
            j.end_object();

            j.begin_member_object("tool");
            json_number(j, "selected", static_cast<uint64_t>(gc_state.selected_tool));
            json_number(j, "current", static_cast<int64_t>(gc_state.current_tool));
            json_bool(j, "offset_available", active_tool.valid);
            json_nullable_number(
                j, "geometry_x_mm", active_tool_data.has_value(), active_tool_data ? active_tool_data->geometry_x_mm : 0.0f);
            json_nullable_number(
                j, "geometry_z_mm", active_tool_data.has_value(), active_tool_data ? active_tool_data->geometry_z_mm : 0.0f);
            json_nullable_number(j, "wear_x_mm", active_tool_data.has_value(), active_tool_data ? active_tool_data->wear_x_mm : 0.0f);
            json_nullable_number(j, "wear_z_mm", active_tool_data.has_value(), active_tool_data ? active_tool_data->wear_z_mm : 0.0f);
            json_number(j, "x_offset_mm", active_tool.x_mm);
            json_number(j, "z_offset_mm", active_tool.z_mm);
            json_number(j, "nose_radius_mm", active_tool.nose_radius_mm);
            json_number(j, "orientation", static_cast<uint64_t>(active_tool.orientation));
            j.end_object();

#ifdef TAMS_MAIJKER_ALARM_ASSETS
            j.begin_member_object("assets");
            j.begin_array("cutting_tools");
            for (uint32_t station = 1; station <= 5; ++station) {
                const auto tool_data = Lathe::get_tool_data(station);
                if (!tool_data.has_value()) {
                    continue;
                }
                const std::string station_text = std::to_string(station);
                const std::string asset_id = "maijker-tool-" + station_text;
                j.begin_object();
                j.member("asset_id", asset_id);
                j.member("tool_id", "T" + station_text);
                // FluidNC does not know a manufacturer serial number. This
                // stable controller-side identity satisfies the required
                // CuttingTool serialNumber without claiming a physical mark.
                j.member("serial_number", asset_id);
                json_number(j, "station", static_cast<uint64_t>(station));
                json_bool(j, "active", active_tool.valid && active_tool.tool_number == station);
                j.member("status", "AVAILABLE");
                json_number(j, "geometry_x_mm", tool_data->geometry_x_mm);
                json_number(j, "geometry_z_mm", tool_data->geometry_z_mm);
                json_number(j, "wear_x_mm", tool_data->wear_x_mm);
                json_number(j, "wear_z_mm", tool_data->wear_z_mm);
                json_number(j, "nose_radius_mm", tool_data->nose_radius_mm);
                json_number(j, "orientation", static_cast<uint64_t>(tool_data->orientation));
                j.end_object();
            }
            j.end_array();
            j.end_object();
#endif

            j.begin_member_object("turret");
            json_bool(j, "configured", turret.configured);
            json_number(j, "station_count", static_cast<uint64_t>(turret.station_count));
            json_number(j, "current_station", static_cast<uint64_t>(turret.current_tool));
            json_number(j, "target_station", static_cast<uint64_t>(turret.target_tool));
            json_bool(j, "software_position_known", turret.tool_confirmed);
            json_bool(j, "sensor_configured", turret.sensor_configured);
            json_bool(j, "sensor_active", turret.sensor_active);
            json_bool(j, "mechanically_confirmed", turret.mechanically_confirmed);
            j.member("position_basis", turret.position_basis);
            j.member("last_error", turret.last_error);
            j.end_object();

            j.begin_member_object("probe");
            json_bool(j, "configured", config->_probe->probePin().defined());
            json_bool(j, "active", config->_probe->probePin().defined() && config->_probe->probePin().get());
            json_bool(j, "cycle_active", probing);
            json_bool(j, "last_succeeded", probe_succeeded);
            j.begin_member_object("last_machine_position");
            json_number(j, "x", last_probe_position[X_AXIS]);
            json_number(j, "z", last_probe_position[Z_AXIS]);
            json_number(j, "c", last_probe_position[C_AXIS]);
            j.end_object();
            j.end_object();

            j.begin_member_object("overrides");
            json_number(j, "feed_percent", static_cast<uint64_t>(sys.f_override()));
            json_number(j, "rapid_percent", static_cast<uint64_t>(sys.r_override()));
            json_number(j, "spindle_percent", static_cast<uint64_t>(sys.spindle_speed_ovr()));
            j.end_object();

            j.begin_member_object("coolant");
            json_bool(j, "flood_available", config->_coolant->hasFlood());
            json_bool(j, "mist_available", config->_coolant->hasMist());
            json_bool(j, "flood_active", coolant.Flood);
            json_bool(j, "mist_active", coolant.Mist);
            j.end_object();

            j.begin_member_object("capabilities");
            json_bool(j, "read_only_snapshot", true);
            json_bool(j, "raw_remote_write", false);
            json_bool(j, "emergency_stop_feedback", estop_configured);
            json_bool(j, "emergency_stop_active", estop_configured && estop_active);
            json_bool(j, "turret_index_feedback", turret.sensor_configured);
            json_bool(j, "feed_hold", true);
            json_bool(j, "jog_cancel", true);
            json_bool(j, "spindle_stop", true);
            json_bool(j, "reset_abort", true);
            json_bool(j, "automatic_resume_after_reconnect", false);
            j.end_object();

            j.begin_array("conditions");
#ifdef TAMS_MAIJKER_ALARM_ASSETS
            if (alarm_state) {
                json_condition(
                    j,
                    alarm_native_severity(lastAlarm),
                    alarm_source(lastAlarm),
                    alarm_native_code(lastAlarm),
                    alarm_name == nullptr ? "Unknown controller alarm" : alarm_name);
            }
#else
            const bool alarm_state =
                sys.state() == State::Alarm ||
                sys.state() == State::Critical ||
                sys.state() == State::ConfigAlarm;
            if (alarm_state) {
                json_condition(
                    j,
                    "FAULT",
                    "CONTROLLER",
                    "SYSTEM_ALARM",
                    alarm_name == nullptr ? "Unknown controller alarm" : alarm_name);
            }
#endif
            if (!estop_configured) {
                json_condition(j, "UNAVAILABLE", "SAFETY", "ESTOP_FEEDBACK_UNAVAILABLE", "Physical E-stop removes power but has no controller feedback input");
            }
            if (!turret.sensor_configured) {
                json_condition(
                    j,
                    "UNAVAILABLE",
                    "TURRET",
                    "TURRET_POSITION_FEEDBACK_UNAVAILABLE",
                    "Turret station is software dead-reckoned and not mechanically confirmed");
            }
            if (Lathe::encoder_enabled() && feedback.stale && spindle_state != SpindleState::Disable) {
                json_condition(j, "WARNING", "SPINDLE", "ENCODER_STALE", "Spindle encoder feedback is stale while the spindle is commanded");
            }
            if (feedback.fault) {
                json_condition(j, "FAULT", "SPINDLE", "ENCODER_FAULT", "Spindle encoder reported a fault");
            }
            if (turret.last_error != nullptr && strcmp(turret.last_error, "ok") != 0) {
                json_condition(j, "WARNING", "TURRET", "TURRET_STATUS", turret.last_error);
            }
            j.end_array();
            j.end();

            if (payload_overflow) {
                log_string(out, "{\"schema\":\"tams.fluidnc.telemetry.v1\",\"error\":\"snapshot_too_large\"}");
                return Error::InvalidValue;
            }
            telemetry_buffer.data()[payload_size] = '\0';
            const char* payload = telemetry_buffer.data();
            void* completion_context = telemetry_buffer.transfer();
            out.sendLineWithCompletion(
                MsgLevelNone,
                payload,
                TelemetryBufferPool::release,
                completion_context);
            return Error::Ok;
        }

        static void send_shared_chuck_mode_response(Channel& out, bool ok, const char* mode, const char* message) {
            std::ostringstream response;
            response << "{\"cmd\":\"426\",\"status\":\"" << (ok ? "ok" : "error") << "\",\"mode\":\"" << mode
                     << "\",\"message\":\"" << message << "\"}";
            log_string(out, response.str());
        }

        static Error selectSharedChuckModeJSON(const char* parameter, AuthenticationLevel auth_level, Channel& out) {  // ESP426
            const std::string request = parameter == nullptr ? "" : parameter;
            Lathe::SharedChuckMode requested_mode = Lathe::SharedChuckMode::Unavailable;
            if (request == "MODE=IDLE") {
                requested_mode = Lathe::SharedChuckMode::Idle;
            } else if (request == "MODE=C_POSITIONING") {
                requested_mode = Lathe::SharedChuckMode::CPositioning;
            } else if (request == "MODE=SPINDLE") {
                requested_mode = Lathe::SharedChuckMode::Spindle;
            } else {
                send_shared_chuck_mode_response(out, false, "UNAVAILABLE", "MODE must be IDLE, C_POSITIONING, or SPINDLE");
                return Error::InvalidValue;
            }

            if (!Lathe::shared_chuck_enabled()) {
                send_shared_chuck_mode_response(out, false, "UNAVAILABLE", "shared chuck is not configured");
                return Error::InvalidValue;
            }
            if (sys.state() != State::Idle || inMotionState() || plan_get_current_block() != nullptr) {
                send_shared_chuck_mode_response(
                    out, false, Lathe::shared_chuck_mode_name(Lathe::shared_chuck_mode()), "controller and planner must be idle");
                return Error::InvalidValue;
            }
            if (spindle->get_state() != SpindleState::Disable) {
                send_shared_chuck_mode_response(
                    out, false, Lathe::shared_chuck_mode_name(Lathe::shared_chuck_mode()), "spindle output must be stopped");
                return Error::InvalidValue;
            }
            if (!Lathe::select_shared_chuck_mode(requested_mode)) {
                send_shared_chuck_mode_response(out, false, "UNAVAILABLE", "mode selection failed");
                return Error::InvalidValue;
            }

            send_shared_chuck_mode_response(out, true, Lathe::shared_chuck_mode_name(requested_mode), "ownership selected; outputs remain stopped");
            return Error::Ok;
        }

        static void send_probe_response(Channel& out, bool ok, char axis, bool contact, float position_mm, const char* message) {
            std::ostringstream response;
            response << "{\"cmd\":\"427\",\"status\":\"" << (ok ? "ok" : "error") << "\",\"axis\":\"" << axis
                     << "\",\"contact\":" << (contact ? "true" : "false") << ",\"position_mm\":" << float_string(position_mm)
                     << ",\"message\":\"" << message << "\"}";
            log_string(out, response.str());
        }

        static Error runBoundedProbeJSON(const char* parameter, AuthenticationLevel auth_level, Channel& out) {  // ESP427
            const auto request = Lathe::parse_bounded_probe_request(parameter == nullptr ? "" : parameter);
            const char axis_name = request.axis == Z_AXIS ? 'Z' : 'X';
            if (request.error != Lathe::BoundedProbeRequestError::None) {
                send_probe_response(out, false, axis_name, false, 0.0f, Lathe::bounded_probe_request_error_message(request.error));
                return Error::InvalidValue;
            }
            if (config->_probe == nullptr || !config->_probe->probePin().defined()) {
                send_probe_response(out, false, axis_name, false, 0.0f, "probe input is not configured");
                return Error::InvalidValue;
            }
            if (sys.state() != State::Idle || inMotionState() || plan_get_current_block() != nullptr) {
                send_probe_response(out, false, axis_name, false, 0.0f, "controller and planner must be idle");
                return Error::InvalidValue;
            }
            if (spindle->get_state() != SpindleState::Disable || Lathe::shared_chuck_mode() != Lathe::SharedChuckMode::Idle) {
                send_probe_response(out, false, axis_name, false, 0.0f, "spindle must be stopped and shared chuck mode must be IDLE");
                return Error::InvalidValue;
            }

            gc_sync_position();
            float target[MAX_N_AXIS] = {};
            copyAxes(target, gc_state.position);
            target[request.axis] += request.distance_mm;

            plan_line_data_t probe_plan = {};
            probe_plan.feed_rate                = request.feed_mm_min;
            probe_plan.spindle                  = SpindleState::Disable;
            probe_plan.spindle_speed            = 0;
            probe_plan.coolant                  = config->_coolant->get_state();
            probe_plan.motion.noFeedOverride    = 1;
            const AxisMask probe_axis            = bitnum_to_mask(request.axis);
            mc_probe_cycle(target, &probe_plan, false, true, probe_axis, __FLT_MAX__);
            gc_sync_position();

            float final_position[MAX_N_AXIS] = {};
            copyAxes(final_position, get_mpos());
            const bool contact = probe_succeeded;
            send_probe_response(
                out,
                contact,
                axis_name,
                contact,
                final_position[request.axis],
                contact ? "probe contact detected" : "probe travel completed without contact");
            return contact ? Error::Ok : Error::GcodeInvalidTarget;
        }

        static Error pairM5DialFromUart(const char* parameter, AuthenticationLevel auth_level, Channel& out) {  // ESP428
            if (strncmp(out.name(), "uart_channel", strlen("uart_channel")) != 0) {
                send_json_command_response(out, 428, false, "physical UART channel required");
                return Error::AuthenticationFailed;
            }

            std::string deviceId;
            std::string fingerprint;
            std::string deviceNonce;
            std::string response;
            if (!get_param(parameter, "D=", deviceId) ||
                !get_param(parameter, "F=", fingerprint) ||
                !get_param(parameter, "N=", deviceNonce) ||
                !DialFirmwareClient::instance().pairFromUart(
                    deviceId, fingerprint, deviceNonce, response)) {
                send_json_command_response(out, 428, false, "invalid UART pairing bootstrap");
                return Error::InvalidValue;
            }
            send_json_command_response(out, 428, true, response);
            return Error::Ok;
        }

        static Error showLatheStatusJSON(const char* parameter, AuthenticationLevel auth_level, Channel& out) {  // ESP421
            spindle->operatorHeartbeat();
            JSONencoder j(&out);
            j.begin();
            j.member("cmd", "421");
            j.member("status", "ok");
            j.begin_array("data");

            j.id_value_object("Lathe enabled", Lathe::enabled() ? "true" : "false");
            const auto homed_axes = Machine::Axes::maskToNames(
                Machine::Axes::homingMask & ~Machine::Homing::unhomed_axes());
            j.id_value_object("Homed axes", homed_axes.c_str());
            j.id_value_object("Spindle state", spindle->isStopping() ? "STOPPING" : spindle_state_name(spindle->get_state()));
            j.id_value_object("Shared chuck mode", Lathe::shared_chuck_mode_name(Lathe::shared_chuck_mode()));
            j.id_value_object("Spindle drive", spindle->driveType());
            j.id_value_object("Spindle commanded RPM", float_string(spindle->commandedRpm()));
            j.id_value_object("Spindle open-loop RPM", float_string(spindle->openLoopRpm()));
            j.id_value_object("Spindle minimum RPM", float_string(spindle->minimumRpm()));
            j.id_value_object("Spindle maximum RPM", float_string(spindle->maximumRpm()));
            j.id_value_object("Spindle acceleration RPM/s", float_string(spindle->accelerationRpmPerSec()));
            j.id_value_object("Spindle deceleration RPM/s", float_string(spindle->decelerationRpmPerSec()));
            j.id_value_object("Spindle stopping", spindle->isStopping() ? "true" : "false");
            j.id_value_object("Spindle stop remaining ms", int32_t(spindle->stopRemainingMs()));
            j.id_value_object("Spindle stop timeouts", int32_t(spindle->stopTimeouts()));
            j.id_value_object("Spindle steps/rev", int32_t(spindle->stepsPerRevolution()));
            j.id_value_object("C position dead reckoned", spindle->positionIsDeadReckoned() ? "true" : "false");
            i2s_out_diagnostics_t i2s_diagnostics = {};
            i2s_out_get_diagnostics(&i2s_diagnostics);
            const auto shared_chuck_mode = Lathe::shared_chuck_mode();
            const char* c_pulse_ownership = Machine::Stepping::continuousFaulted()
                                                ? "FAULT"
                                                : shared_chuck_mode == Lathe::SharedChuckMode::Spindle
                                                      ? "SPINDLE"
                                                      : strcmp(spindle->cReferenceName(), "PENDING_INDEX_HANDOFF") == 0
                                                            ? "TRANSITION"
                                                            : "POSITIONING";
            j.id_value_object("I2S FIFO threshold", int32_t(i2s_diagnostics.fifo_threshold));
            j.id_value_object("I2S FIFO reload", int32_t(i2s_diagnostics.fifo_reload));
            j.id_value_object("I2S underruns", int32_t(i2s_diagnostics.underruns));
            j.id_value_object("I2S max ISR gap us", int32_t(i2s_diagnostics.max_isr_gap_us));
            j.id_value_object("I2S max ISR duration us", int32_t(i2s_diagnostics.max_isr_duration_us));
            j.id_value_object("C pulse requested Hz", float_string(Machine::Stepping::continuousTargetRateMillihz() / 1000.0f));
            j.id_value_object("C pulse scheduled Hz", float_string(Machine::Stepping::continuousRateMillihz() / 1000.0f));
            j.id_value_object("C pulse emitted count", int32_t(Machine::Stepping::continuousPulseCount()));
            j.id_value_object("C pulse ownership", c_pulse_ownership);
            j.id_value_object("C reference", spindle->cReferenceName());
            j.id_value_object("Threading enabled", Lathe::feature_enabled(Lathe::Feature::Threading) ? "true" : "false");
            j.id_value_object("Spindle speed mode", lathe_spindle_mode_name());
            j.id_value_object("Diameter mode", lathe_diameter_mode_name());
            j.id_value_object("Feed mode", feed_mode_name());
            j.id_value_object("Programmed S", float_string(gc_state.spindle_speed));
            j.id_value_object("Effective RPM", float_string(gc_state.lathe_commanded_rpm));
            j.id_value_object("CSS clamp RPM", float_string(Lathe::max_css_rpm()));
            j.id_value_object("Minimum CSS diameter mm", float_string(Lathe::min_css_diameter_mm()));
            j.id_value_object("Encoder enabled", Lathe::encoder_enabled() ? "true" : "false");
            j.id_value_object("Encoder capture active", Lathe::encoder_capture_active() ? "true" : "false");
            j.id_value_object("Encoder pulses/rev", int32_t(Lathe::encoder_pulses_per_revolution()));

            auto tool = Lathe::active_tool_offset();
            j.id_value_object("Active lathe tool", int32_t(tool.tool_number));
            j.id_value_object("Lathe tool X offset mm", float_string(tool.x_mm));
            j.id_value_object("Lathe tool Z offset mm", float_string(tool.z_mm));
            j.id_value_object("Tool nose radius mm", float_string(tool.nose_radius_mm));

            auto turret = ATCs::maijker_turret_status();
            j.id_value_object("Turret configured", turret.configured ? "true" : "false");
            j.id_value_object("Turret station count", int32_t(turret.station_count));
            j.id_value_object("Turret current tool", int32_t(turret.current_tool));
            j.id_value_object("Turret target tool", int32_t(turret.target_tool));
            j.id_value_object("Turret software position known", turret.tool_confirmed ? "true" : "false");
            j.id_value_object("Turret mechanically confirmed", turret.mechanically_confirmed ? "true" : "false");
            j.id_value_object("Turret position basis", turret.position_basis);
            j.id_value_object("Turret sensor configured", turret.sensor_configured ? "true" : "false");
            j.id_value_object("Turret sensor active", turret.sensor_active ? "true" : "false");
            j.id_value_object("Turret last error", turret.last_error);

            const auto feedback = spindle->latheFeedback().status();
            j.id_value_object("Feedback measured RPM", feedback.has_measured_rpm ? float_string(feedback.measured_rpm) : "not available");
            j.id_value_object("Feedback index", feedback.has_index_pulse ? "true" : "false");
            j.id_value_object("Feedback angular position", feedback.has_angular_position ? "true" : "false");
            j.id_value_object("Feedback indexed angle", feedback.has_indexed_angle ? "true" : "false");
            j.id_value_object("Feedback angular rev", feedback.has_angular_position ? float_string(feedback.angular_position_rev) : "not available");
            j.id_value_object("Feedback revolution count", int32_t(feedback.revolution_count));
            j.id_value_object("Feedback stale", feedback.stale ? "true" : "false");
            j.id_value_object("Feedback fault", feedback.fault ? "true" : "false");
            j.id_value_object("Threading feedback ready", Lathe::feedback_supports_threading(feedback) ? "true" : "false");

            j.end_array();
            j.end();
            return Error::Ok;
        }

        static Error showLatheLiveStatusJSON(const char* parameter, AuthenticationLevel auth_level, Channel& out) {  // ESP430
            // The pendant polls this compact document while C is rotating. It
            // keeps the spindle operator watchdog alive without formatting and
            // transmitting the full configuration/diagnostics document once
            // per second.
            spindle->operatorHeartbeat();
            JSONencoder j(&out);
            j.begin();
            j.member("cmd", "430");
            j.member("status", "ok");
            j.begin_array("data");
            j.id_value_object("Lathe enabled", Lathe::enabled() ? "true" : "false");
            j.id_value_object("Spindle state", spindle->isStopping() ? "STOPPING" : spindle_state_name(spindle->get_state()));
            j.id_value_object("Shared chuck mode", Lathe::shared_chuck_mode_name(Lathe::shared_chuck_mode()));
            j.id_value_object("Spindle commanded RPM", float_string(spindle->commandedRpm()));
            j.id_value_object("Spindle open-loop RPM", float_string(spindle->openLoopRpm()));
            j.id_value_object("Spindle stopping", spindle->isStopping() ? "true" : "false");
            j.id_value_object("Spindle stop remaining ms", int32_t(spindle->stopRemainingMs()));
            const auto feedback = spindle->latheFeedback().status();
            j.id_value_object("Feedback measured RPM", feedback.has_measured_rpm ? float_string(feedback.measured_rpm) : "not available");
            j.id_value_object("Feedback indexed angle", feedback.has_indexed_angle ? "true" : "false");
            j.id_value_object("Feedback angular rev", feedback.has_angular_position ? float_string(feedback.angular_position_rev) : "not available");
            j.id_value_object("Feedback stale", feedback.stale ? "true" : "false");
            j.id_value_object("Feedback fault", feedback.fault ? "true" : "false");
            j.end_array();
            j.end();
            return Error::Ok;
        }

        static bool get_float_param(const char* parameter, const char* key, float& value) {
            std::string text;
            if (!get_param(parameter, key, text)) {
                return false;
            }
            char* end = nullptr;
            value = strtof(text.c_str(), &end);
            return end != text.c_str();
        }

        static bool get_uint_param(const char* parameter, const char* key, uint32_t& value) {
            std::string text;
            if (!get_param(parameter, key, text)) {
                return false;
            }
            char* end = nullptr;
            value = strtoul(text.c_str(), &end, 10);
            return end != text.c_str();
        }

        static Error setLatheToolJSON(const char* parameter, AuthenticationLevel auth_level, Channel& out) {  // ESP422
            uint32_t tool_number = 0;
            if (!get_uint_param(parameter, "T=", tool_number) || tool_number == 0) {
                send_json_command_response(out, 422, false, errorString(Error::InvalidValue));
                return Error::InvalidValue;
            }

            Lathe::ToolData tool;
            if (auto existing = Lathe::get_tool_data(tool_number)) {
                tool = *existing;
            }

            get_float_param(parameter, "GX=", tool.geometry_x_mm);
            get_float_param(parameter, "GZ=", tool.geometry_z_mm);
            get_float_param(parameter, "WX=", tool.wear_x_mm);
            get_float_param(parameter, "WZ=", tool.wear_z_mm);
            get_float_param(parameter, "NR=", tool.nose_radius_mm);

            uint32_t orientation = 0;
            if (get_uint_param(parameter, "O=", orientation)) {
                tool.orientation = static_cast<Lathe::InsertOrientation>(orientation);
            }

            Lathe::set_tool_data(tool_number, tool);
            send_json_command_response(out, 422, true, "lathe tool saved");
            return Error::Ok;
        }

        static Error touchOffLatheToolJSON(const char* parameter, AuthenticationLevel auth_level, Channel& out) {  // ESP423
            Lathe::TouchOffSpec spec;
            if (!get_uint_param(parameter, "T=", spec.tool_number) || spec.tool_number == 0) {
                send_json_command_response(out, 423, false, errorString(Error::InvalidValue));
                return Error::InvalidValue;
            }

            spec.set_x = get_float_param(parameter, "MX=", spec.machine_x_mm) && get_float_param(parameter, "RX=", spec.reference_x_mm);
            spec.set_z = get_float_param(parameter, "MZ=", spec.machine_z_mm) && get_float_param(parameter, "RZ=", spec.reference_z_mm);

            std::string mode;
            if (get_param(parameter, "MODE=", mode) && (mode == "diameter" || mode == "G7" || mode == "g7")) {
                spec.x_mode = Lathe::DiameterMode::Diameter;
            }

            Error err = Lathe::touch_off_tool(spec);
            send_json_command_response(out, 423, err == Error::Ok, err == Error::Ok ? "lathe tool touched off" : errorString(err));
            return err;
        }

        static Error homeMaijkerTurretJSON(const char* parameter, AuthenticationLevel auth_level, Channel& out) {  // ESP424
            std::string home;
            if (!get_param(parameter, "HOME=", home) || !(home == "1" || home == "true" || home == "TRUE")) {
                send_json_command_response(out, 424, false, errorString(Error::InvalidValue));
                return Error::InvalidValue;
            }

            Error err = ATCs::maijker_turret_home();
            auto  status = ATCs::maijker_turret_status();
            send_json_command_response(out, 424, err == Error::Ok, status.last_error);
            return err;
        }

        static Error showSysStats(const char* parameter, AuthenticationLevel auth_level, Channel& out) {  // ESP420
            if (paramIsJSON(parameter)) {
                return showSysStatsJSON(parameter, auth_level, out);
            }

            log_stream(out, "Chip ID: " << (uint16_t)(ESP.getEfuseMac() >> 32));
            log_stream(out, "CPU Cores: " << ESP.getChipCores());
            log_stream(out, "CPU Frequency: " << ESP.getCpuFreqMHz() << "Mhz");

            std::ostringstream msg;
            msg << std::fixed << std::setprecision(1) << temperatureRead() << "°C";
            log_stream(out, "CPU Temperature: " << msg.str());
            log_stream(out, "Free memory: " << formatBytes(ESP.getFreeHeap()));
            log_stream(out, "SDK: " << ESP.getSdkVersion());
            log_stream(out, "Flash Size: " << formatBytes(ESP.getFlashChipSize()));

            for (auto const& module : Modules()) {
                module->build_info(out);
            }

            log_stream(out, "FW version: FluidNC " << git_info);
            return Error::Ok;
        }

        static Error setWebSetting(const char* parameter, AuthenticationLevel auth_level, Channel& out) {  // ESP401
            // The string is of the form "P=name T=type V=value
            // We do not need the "T=" (type) parameter because the
            // Setting objects know their own type.  We do not use
            // split_params because if fails if the value string
            // contains '='
            std::string p, v;
            bool        isJSON = paramIsJSON(parameter);
            if (!(get_param(parameter, "P=", p) && get_param(parameter, "V=", v))) {
                if (isJSON) {
                    send_json_command_response(out, 401, false, errorString(Error::InvalidValue));
                }
                return Error::InvalidValue;
            }

            Error ret = do_command_or_setting(p, v, auth_level, out);
            if (isJSON) {
                send_json_command_response(out, 401, ret == Error::Ok, errorString(ret));
            }

            return ret;
        }

        // Used by js/setting.js
        static Error listSettingsJSON(const char* parameter, AuthenticationLevel auth_level, Channel& out) {  // ESP400
            JSONencoder j(&out);
            j.begin();
            j.member("cmd", "400");
            j.member("status", "ok");
            j.begin_array("data");

            // NVS settings
            j.setCategory("Flash/Settings");
            for (Setting* js : Setting::List) {
                js->addWebui(&j);
            }

            // Configuration tree
            j.setCategory("Running/Config");
            Configuration::JsonGenerator gen(j);
            config->group(gen);

            j.end_array();
            j.end();

            return Error::Ok;
        }

        static Error listSettings(const char* parameter, AuthenticationLevel auth_level, Channel& out) {  // ESP400
            if (parameter != NULL) {
                if (strstr(parameter, "json=yes") != NULL) {
                    return listSettingsJSON(parameter, auth_level, out);
                }
            }

            JSONencoder j(&out);

            j.begin();
            j.begin_array("EEPROM");

            // NVS settings
            j.setCategory("nvs");
            for (Setting* js : Setting::List) {
                js->addWebui(&j);
            }

            // Configuration tree
            j.setCategory("tree");
            Configuration::JsonGenerator gen(j);
            config->group(gen);

            j.end_array();
            j.end();

            return Error::Ok;
        }

        static Error showWebHelp(const char* parameter, AuthenticationLevel auth_level, Channel& out) {  // ESP0
            log_string(out, "Persistent web settings - $name to show, $name=value to set");
            log_string(out, "ESPname FullName         Description");
            log_string(out, "------- --------         -----------");

            for (Setting* setting : Setting::List) {
                if (setting->getType() == WEBSET) {
                    log_stream(out,
                               LeftJustify(setting->getGrblName() ? setting->getGrblName() : "", 8)
                                   << LeftJustify(setting->getName(), 25 - 8) << setting->getDescription());
                }
            }
            log_string(out, "");
            log_string(out, "Other web commands: $name to show, $name=value to set");
            log_string(out, "ESPname FullName         Values");
            log_string(out, "------- --------         ------");

            for (Command* cp : Command::List) {
                if (cp->getType() == WEBCMD) {
                    LogStream s(out, "");
                    s << LeftJustify(cp->getGrblName() ? cp->getGrblName() : "", 8) << LeftJustify(cp->getName(), 25 - 8);
                    if (cp->getDescription()) {
                        s << cp->getDescription();
                    }
                }
            }
            return Error::Ok;
        }

        void init() override {
            // If authentication enabled, display_settings skips or displays <Authentication Required>
            // RU - need user or admin password to read
            // WU - need user or admin password to set
            // WA - need admin password to set
            new WebCommand(NULL, WEBCMD, WU, "ESP420", "System/Stats", showSysStats, anyState);
            new WebCommand(NULL, WEBCMD, WU, "ESP421", "System/Lathe", showLatheStatusJSON, anyState);
            new WebCommand(NULL, WEBCMD, WU, "ESP430", "System/LatheLive", showLatheLiveStatusJSON, anyState);
            // Physical serial commands run as guest. ESP425 is deliberately
            // read-only, so keep it guest-readable without weakening the
            // administrator requirement on the bounded control commands.
            new WebCommand(NULL, WEBCMD, WG, "ESP425", "System/TamsTelemetry", showTamsTelemetryJSON, anyState);
            new WebCommand("MODE=IDLE|C_POSITIONING|SPINDLE", WEBCMD, WA, "ESP426", "Lathe/SharedChuckMode", selectSharedChuckModeJSON, anyState);
            new WebCommand(
                "PROBE,AXIS=X|Z,DISTANCE=signed_mm,FEED=mm_per_min", WEBCMD, WA, "ESP427", "Lathe/BoundedProbe", runBoundedProbeJSON, anyState);
            new WebCommand(
                "D=device_id F=identity_fingerprint N=device_nonce", WEBCMD, WG, "ESP428", "System/M5DialUartPair", pairM5DialFromUart, anyState);
            new WebCommand("T=tool [GX=x] [GZ=z] [WX=x] [WZ=z] [NR=r] [O=orientation]", WEBCMD, WA, "ESP422", "Lathe/ToolSet", setLatheToolJSON, anyState);
            new WebCommand("T=tool [MX=x RX=x MODE=diameter|radius] [MZ=z RZ=z]", WEBCMD, WA, "ESP423", "Lathe/TouchOff", touchOffLatheToolJSON, anyState);
            new WebCommand("HOME=1", WEBCMD, WA, "ESP424", "Lathe/TurretHome", homeMaijkerTurretJSON, anyState);
            new WebCommand("RESTART", WEBCMD, WA, "ESP444", "System/Control", setSystemMode);

            //      new WebCommand("ON|OFF", WEBCMD, WA, "ESP115", "Radio/State", setRadioState);

            new WebCommand("P=position T=type V=value", WEBCMD, WA, "ESP401", "WebUI/Set", setWebSetting);
            new WebCommand(NULL, WEBCMD, WU, "ESP400", "WebUI/List", listSettings, anyState);
            new WebCommand(NULL, WEBCMD, WG, "ESP0", "WebUI/Help", showWebHelp, anyState);
            new WebCommand(NULL, WEBCMD, WG, "ESP", "WebUI/Help", showWebHelp, anyState);
        }
    };
    ModuleFactory::InstanceBuilder<WebCommands> web_commands_module __attribute__((init_priority(103))) ("web_commands", true);
}
