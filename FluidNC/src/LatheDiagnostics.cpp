#include "LatheDiagnostics.h"

#include "Machine/Axes.h"
#include "Machine/MachineConfig.h"
#include "Driver/i2s_out.h"
#include "Lathe.h"
#include "Planner.h"
#include "State.h"
#include "Stepper.h"
#include "Stepping.h"

#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <Arduino.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>

namespace LatheDiagnostics {
    namespace {
        constexpr size_t EventCount = 16;

        struct Event {
            uint32_t sequence = 0;
            uint32_t timestampMs = 0;
            int      result = 0;
            char     kind[20] = {};
            char     source[28] = {};
            char     detail[96] = {};
        };

        portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
        std::array<Event, EventCount> events;
        uint32_t eventCursor = 0;
        uint32_t lineCount = 0;
        uint32_t jogCount = 0;
        uint32_t homeCount = 0;
        uint32_t spindleCount = 0;
        uint32_t statusQueryCount = 0;
        uint32_t realtimeStatusCount = 0;
        uint32_t resetCount = 0;
        uint32_t jogCancelCount = 0;
        uint32_t stepperWakeCount = 0;
        uint32_t stepperIdleCount = 0;
        uint32_t lastStepperWakeMs = 0;
        uint32_t lastStepperIdleMs = 0;

        bool containsInsensitive(const char* text, const char* needle) {
            if (!text || !needle) return false;
            const size_t needleLength = strlen(needle);
            for (const char* start = text; *start; ++start) {
                size_t i = 0;
                while (i < needleLength && start[i] &&
                       std::tolower(static_cast<unsigned char>(start[i])) ==
                           std::tolower(static_cast<unsigned char>(needle[i]))) {
                    ++i;
                }
                if (i == needleLength) return true;
            }
            return false;
        }

        bool sensitive(const char* line) {
            return containsInsensitive(line, "password") ||
                   containsInsensitive(line, "secret") ||
                   containsInsensitive(line, "token") ||
                   containsInsensitive(line, "ssid") ||
                   containsInsensitive(line, "$sta/");
        }

        void appendEvent(const char* kind, const char* source, const char* detail, int result) {
            portENTER_CRITICAL(&mux);
            Event& event = events[eventCursor % EventCount];
            event.sequence = ++eventCursor;
            event.timestampMs = millis();
            event.result = result;
            strlcpy(event.kind, kind ? kind : "unknown", sizeof(event.kind));
            strlcpy(event.source, source ? source : "unknown", sizeof(event.source));
            strlcpy(event.detail, detail ? detail : "", sizeof(event.detail));
            portEXIT_CRITICAL(&mux);
        }

        std::string escape(const char* input) {
            std::string out;
            for (const unsigned char* p =
                     reinterpret_cast<const unsigned char*>(input ? input : "");
                 *p;
                 ++p) {
                if (*p == '"' || *p == '\\') out += '\\';
                if (*p >= 0x20) out += static_cast<char>(*p);
            }
            return out;
        }

        void appendFixed6(std::string& out, double value) {
            const bool   negative  = value < 0.0;
            const double magnitude = negative ? -value : value;
            if (!std::isfinite(magnitude) ||
                magnitude > static_cast<double>(std::numeric_limits<uint64_t>::max() / 1000000ULL)) {
                out += "null";
                return;
            }

            const uint64_t scaled = static_cast<uint64_t>(magnitude * 1000000.0 + 0.5);
            const uint64_t whole  = scaled / 1000000ULL;
            const uint32_t fraction = static_cast<uint32_t>(scaled % 1000000ULL);

            if (negative) out += '-';
            out += std::to_string(whole);
            out += '.';

            char     digits[6];
            uint32_t remaining = fraction;
            for (int index = 5; index >= 0; --index) {
                digits[index] = static_cast<char>('0' + (remaining % 10));
                remaining /= 10;
            }
            out.append(digits, sizeof(digits));
        }
    }

    void recordLine(const char* source, const char* line, Error result) {
        if (!line || !*line) return;
        ++lineCount;
        if (strncmp(line, "[ESP421]", 8) == 0 || strcmp(line, "ESP421") == 0) {
            ++statusQueryCount;
            return;
        }

        const char* kind = "command";
        if (strncmp(line, "$J=", 3) == 0) {
            kind = "jog";
            ++jogCount;
        } else if (strncmp(line, "$H", 2) == 0) {
            kind = "home";
            ++homeCount;
        } else if (strncmp(line, "M3", 2) == 0 || strncmp(line, "M4", 2) == 0 ||
                   strncmp(line, "M5", 2) == 0) {
            kind = "spindle";
            ++spindleCount;
        } else if (line[0] == 'G' || line[0] == 'g') {
            kind = "gcode";
        }
        appendEvent(kind, source, sensitive(line) ? "[redacted]" : line,
                    static_cast<int>(result));
    }

    void recordRealtime(const char* source, uint32_t command) {
        if (command == static_cast<uint32_t>('?')) {
            ++realtimeStatusCount;
            return;
        }
        char detail[20];
        snprintf(detail, sizeof(detail), "0x%02lx",
                 static_cast<unsigned long>(command));
        const char* kind = "realtime";
        if (command == 0x18) {
            kind = "reset";
            ++resetCount;
        } else if (command == 0x85) {
            kind = "jog_cancel";
            ++jogCancelCount;
        }
        appendEvent(kind, source, detail, 0);
    }

    void recordStepperWake() {
        ++stepperWakeCount;
        lastStepperWakeMs = millis();
    }

    void recordStepperIdle() {
        ++stepperIdleCount;
        lastStepperIdleMs = millis();
    }

    std::string snapshotJson() {
        uint32_t cursor;
        portENTER_CRITICAL(&mux);
        cursor = eventCursor;
        portEXIT_CRITICAL(&mux);

        i2s_out_diagnostics_t i2s = {};
        i2s_out_get_diagnostics(&i2s);
        const axis_t cAxis = Lathe::c_axis();

        std::string json;
        json.reserve(4096);
        json += "{\"schema_version\":1,\"device\":\"dlc32\",\"uptime_ms\":";
        json += std::to_string(millis());
        json += ",\"reset_reason\":";
        json += std::to_string(static_cast<int>(esp_reset_reason()));
        json += ",\"free_heap\":";
        json += std::to_string(xPortGetFreeHeapSize());
        json += ",\"state\":\"";
        json += escape(state_name());
        json += "\",\"planner_busy\":";
        json += plan_get_current_block() ? "true" : "false";
        json += ",\"steppers\":{\"awake\":";
        json += Stepper::is_awake() ? "true" : "false";
        json += ",\"drivers_disabled\":";
        json += Machine::Axes::disabled ? "true" : "false";
        json += ",\"idle_ms\":";
        json += std::to_string(Stepping::_idleMsecs);
        json += ",\"wake_count\":";
        json += std::to_string(stepperWakeCount);
        json += ",\"idle_count\":";
        json += std::to_string(stepperIdleCount);
        json += ",\"last_wake_ms\":";
        json += std::to_string(lastStepperWakeMs);
        json += ",\"last_idle_ms\":";
        json += std::to_string(lastStepperIdleMs);
        json += "},\"i2s\":{\"underruns\":";
        json += std::to_string(i2s.underruns);
        json += ",\"max_isr_gap_us\":";
        json += std::to_string(i2s.max_isr_gap_us);
        json += ",\"max_isr_duration_us\":";
        json += std::to_string(i2s.max_isr_duration_us);
        json += ",\"planner_active\":";
        json += i2s.planner_active ? "true" : "false";
        json += ",\"planner_interval_ticks\":";
        json += std::to_string(i2s.planner_interval_ticks);
        json += ",\"planner_interval_frames\":";
        json += std::to_string(i2s.planner_interval_frames);
        json += ",\"planner_fractional_residual_ticks\":";
        json += std::to_string(i2s.planner_fractional_residual_ticks);
        json += ",\"planner_scheduled_ticks\":";
        json += std::to_string(i2s.planner_scheduled_ticks);
        json += ",\"planner_emitted_frames\":";
        json += std::to_string(i2s.planner_emitted_frames);
        json += ",\"planner_emitted_intervals\":";
        json += std::to_string(i2s.planner_emitted_intervals);
        json += ",\"transport_faulted\":";
        json += i2s.transport_faulted ? "true" : "false";
        json += "},\"c_planner\":{\"steps\":";
        json += std::to_string(Machine::Stepping::getSteps(cAxis));
        json += ",\"position_degrees\":";
        appendFixed6(json, get_mpos()[cAxis]);
        json += ",\"continuous_active\":";
        json += Machine::Stepping::continuousActive() ? "true" : "false";
        json += ",\"continuous_target_millihz\":";
        json += std::to_string(Machine::Stepping::continuousTargetRateMillihz());
        json += ",\"continuous_scheduled_millihz\":";
        json += std::to_string(Machine::Stepping::continuousRateMillihz());
        json += ",\"continuous_pulses\":";
        json += std::to_string(Machine::Stepping::continuousPulseCount());
        json += ",\"continuous_faulted\":";
        json += Machine::Stepping::continuousFaulted() ? "true" : "false";
        json += "},\"input\":{\"line_count\":";
        json += std::to_string(lineCount);
        json += ",\"jog_count\":";
        json += std::to_string(jogCount);
        json += ",\"home_count\":";
        json += std::to_string(homeCount);
        json += ",\"spindle_count\":";
        json += std::to_string(spindleCount);
        json += ",\"lathe_status_queries\":";
        json += std::to_string(statusQueryCount);
        json += ",\"realtime_status_queries\":";
        json += std::to_string(realtimeStatusCount);
        json += ",\"reset_count\":";
        json += std::to_string(resetCount);
        json += ",\"jog_cancel_count\":";
        json += std::to_string(jogCancelCount);
        json += "},\"recent_events\":[";

        const uint32_t count = std::min<uint32_t>(cursor, EventCount);
        uint32_t emitted = 0;
        for (uint32_t offset = 0; offset < count; ++offset) {
            const uint32_t sequence = cursor - offset;
            Event event;
            portENTER_CRITICAL(&mux);
            event = events[(sequence - 1) % EventCount];
            portEXIT_CRITICAL(&mux);
            if (event.sequence != sequence) continue;
            if (emitted++) json += ",";
            json += "{\"sequence\":" + std::to_string(event.sequence) +
                    ",\"timestamp_ms\":" + std::to_string(event.timestampMs) +
                    ",\"kind\":\"" + escape(event.kind) + "\",\"source\":\"" +
                    escape(event.source) + "\",\"detail\":\"" +
                    escape(event.detail) + "\",\"result\":" +
                    std::to_string(event.result) + "}";
        }
        json += "]}";
        return json;
    }
}
