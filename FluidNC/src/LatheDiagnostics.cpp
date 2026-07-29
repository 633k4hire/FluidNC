#include "LatheDiagnostics.h"

#include "Machine/Axes.h"
#include "Machine/MachineConfig.h"
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
#include <cstdio>
#include <cstring>

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
        std::array<Event, EventCount> copy;
        uint32_t cursor;
        portENTER_CRITICAL(&mux);
        copy = events;
        cursor = eventCursor;
        portEXIT_CRITICAL(&mux);

        std::string json =
            "{\"schema_version\":1,\"device\":\"dlc32\",\"uptime_ms\":" +
            std::to_string(millis()) + ",\"reset_reason\":" +
            std::to_string(static_cast<int>(esp_reset_reason())) +
            ",\"free_heap\":" + std::to_string(xPortGetFreeHeapSize()) +
            ",\"state\":\"" + escape(state_name()) + "\",\"planner_busy\":" +
            (plan_get_current_block() ? "true" : "false") +
            ",\"steppers\":{\"awake\":" + (Stepper::is_awake() ? "true" : "false") +
            ",\"drivers_disabled\":" + (Machine::Axes::disabled ? "true" : "false") +
            ",\"idle_ms\":" + std::to_string(Stepping::_idleMsecs) +
            ",\"wake_count\":" + std::to_string(stepperWakeCount) +
            ",\"idle_count\":" + std::to_string(stepperIdleCount) +
            ",\"last_wake_ms\":" + std::to_string(lastStepperWakeMs) +
            ",\"last_idle_ms\":" + std::to_string(lastStepperIdleMs) +
            "},\"input\":{\"line_count\":" + std::to_string(lineCount) +
            ",\"jog_count\":" + std::to_string(jogCount) +
            ",\"home_count\":" + std::to_string(homeCount) +
            ",\"spindle_count\":" + std::to_string(spindleCount) +
            ",\"lathe_status_queries\":" + std::to_string(statusQueryCount) +
            ",\"realtime_status_queries\":" + std::to_string(realtimeStatusCount) +
            ",\"reset_count\":" + std::to_string(resetCount) +
            ",\"jog_cancel_count\":" + std::to_string(jogCancelCount) +
            "},\"recent_events\":[";

        const uint32_t count = std::min<uint32_t>(cursor, EventCount);
        for (uint32_t offset = 0; offset < count; ++offset) {
            const uint32_t sequence = cursor - offset;
            const Event& event = copy[(sequence - 1) % EventCount];
            if (offset) json += ",";
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
