// Copyright (c) 2026 FluidNC contributors
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "Lathe.h"

#include "Machine/MachineConfig.h"
#include "Logging.h"
#include "Protocol.h"
#include "Settings.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <cstdlib>
#include <cstring>

#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
#    include <Arduino.h>
#endif

namespace Lathe {
    namespace {
        struct ToolSlot {
            uint32_t tool_number = 0;
            ToolData data;
        };

        constexpr size_t MaxLatheTools = 32;
        constexpr const char* ToolTableNvsKey = "LatheTools";
        std::array<ToolSlot, MaxLatheTools> tool_table;
        ActiveToolOffset active_offset;
        bool tool_table_loaded = false;
        volatile SharedChuckMode active_shared_chuck_mode = SharedChuckMode::Idle;
        std::string last_program_name;

        void reset_tool_table() {
            tool_table = {};
            active_offset = {};
        }

        void ensure_tool_table_loaded() {
            if (!tool_table_loaded) {
                load_tool_table();
            }
        }
    }

    bool enabled() {
        return config != nullptr && config->_lathe != nullptr && config->_lathe->_enable;
    }

    bool feature_enabled(Feature feature) {
        if (!enabled()) {
            return false;
        }

        switch (feature) {
            case Feature::ConstantSurfaceSpeed:
                return config->_lathe->_enableCSS;
            case Feature::FeedPerRevolution:
                return config->_lathe->_enableFeedPerRev;
            case Feature::Threading:
                return config->_lathe->_enableThreading;
        }
        return false;
    }

    const char* feature_name(Feature feature) {
        switch (feature) {
            case Feature::ConstantSurfaceSpeed:
                return "constant surface speed";
            case Feature::FeedPerRevolution:
                return "feed per revolution";
            case Feature::Threading:
                return "threading";
        }
        return "lathe feature";
    }

    const char* unsupported_message(Feature feature) {
        switch (feature) {
            case Feature::ConstantSurfaceSpeed:
                return "Lathe constant surface speed (G96/G97) is scaffolded but not implemented";
            case Feature::FeedPerRevolution:
                return "Lathe feed per revolution (G95) is scaffolded but not implemented";
            case Feature::Threading:
                return "Lathe threading is scaffolded but not implemented";
        }
        return "Lathe feature is scaffolded but not implemented";
    }

    float max_css_rpm() {
        return enabled() ? config->_lathe->_maxCssRpm : 0.0f;
    }

    float min_css_diameter_mm() {
        return enabled() ? config->_lathe->_minCssDiameter : 1.0f;
    }

    bool encoder_enabled() {
        return enabled() && config->_lathe->_encoderEnable;
    }

    uint32_t encoder_pulses_per_revolution() {
        return encoder_enabled() ? config->_lathe->_encoderPulsesPerRev : 1;
    }

    axis_t x_axis() {
        return enabled() ? static_cast<axis_t>(config->_lathe->_xAxis) : X_AXIS;
    }

    axis_t z_axis() {
        return enabled() ? static_cast<axis_t>(config->_lathe->_zAxis) : Z_AXIS;
    }

    bool shared_chuck_enabled() {
        return enabled() && config->_lathe->_sharedChuck;
    }

    axis_t c_axis() {
        return shared_chuck_enabled() ? static_cast<axis_t>(config->_lathe->_cAxis) : C_AXIS;
    }

    SharedChuckDecision evaluate_shared_chuck_transition(
        bool shared_enabled, SharedChuckMode current_mode, bool c_axis_motion, SpindleState requested_spindle) {
        if (!shared_enabled) {
            return { SharedChuckDisposition::Allow, SharedChuckMode::Unavailable, SharedChuckConflict::None };
        }

        const bool spindle_requested = requested_spindle != SpindleState::Disable;
        if (c_axis_motion && spindle_requested) {
            return {
                SharedChuckDisposition::Reject,
                current_mode,
                SharedChuckConflict::SimultaneousSpindleAndCAxis,
            };
        }
        if (c_axis_motion) {
            return { SharedChuckDisposition::Allow, SharedChuckMode::CPositioning, SharedChuckConflict::None };
        }
        if (spindle_requested) {
            const auto disposition = current_mode == SharedChuckMode::CPositioning
                                         ? SharedChuckDisposition::AllowAfterSynchronize
                                         : SharedChuckDisposition::Allow;
            return { disposition, SharedChuckMode::Spindle, SharedChuckConflict::None };
        }
        if (current_mode == SharedChuckMode::Spindle) {
            return { SharedChuckDisposition::Allow, SharedChuckMode::Idle, SharedChuckConflict::None };
        }
        return { SharedChuckDisposition::Allow, current_mode, SharedChuckConflict::None };
    }

    SharedChuckMode shared_chuck_mode() {
        return shared_chuck_enabled() ? active_shared_chuck_mode : SharedChuckMode::Unavailable;
    }

    const char* shared_chuck_mode_name(SharedChuckMode mode) {
        switch (mode) {
            case SharedChuckMode::Idle:
                return "IDLE";
            case SharedChuckMode::CPositioning:
                return "C_POSITIONING";
            case SharedChuckMode::Spindle:
                return "SPINDLE";
            case SharedChuckMode::Unavailable:
                return "UNAVAILABLE";
        }
        return "UNAVAILABLE";
    }

    const char* shared_chuck_conflict_message(SharedChuckConflict conflict) {
        switch (conflict) {
            case SharedChuckConflict::SimultaneousSpindleAndCAxis:
                return "Shared chuck cannot run the spindle and C axis in the same block; stop the spindle with M5 before C positioning";
            case SharedChuckConflict::None:
                return "ok";
        }
        return "unknown shared chuck conflict";
    }

    BoundedProbeRequest parse_bounded_probe_request(const std::string& request) {
        BoundedProbeRequest result;
        constexpr const char* Prefix = "PROBE,AXIS=";
        constexpr const char* DistanceKey = ",DISTANCE=";
        constexpr const char* FeedKey = ",FEED=";
        constexpr size_t MaxRequestLength = 96;

        if (request.empty() || request.size() > MaxRequestLength || request.rfind(Prefix, 0) != 0) {
            return result;
        }

        const size_t axis_begin = strlen(Prefix);
        const size_t distance_key = request.find(DistanceKey, axis_begin);
        if (distance_key == std::string::npos || distance_key != axis_begin + 1) {
            result.error = BoundedProbeRequestError::InvalidAxis;
            return result;
        }
        const char axis_name = request[axis_begin];
        if (axis_name == 'X') {
            result.axis = X_AXIS;
        } else if (axis_name == 'Z') {
            result.axis = Z_AXIS;
        } else {
            result.error = BoundedProbeRequestError::InvalidAxis;
            return result;
        }

        const size_t distance_begin = distance_key + strlen(DistanceKey);
        const size_t feed_key = request.find(FeedKey, distance_begin);
        if (feed_key == std::string::npos || request.find(',', feed_key + 1) != std::string::npos) {
            result.error = BoundedProbeRequestError::Malformed;
            return result;
        }

        auto parse_finite = [](const std::string& value, float& parsed) {
            if (value.empty()) {
                return false;
            }
            char* end = nullptr;
            parsed = strtof(value.c_str(), &end);
            return end != value.c_str() && *end == '\0' && std::isfinite(parsed);
        };

        if (!parse_finite(request.substr(distance_begin, feed_key - distance_begin), result.distance_mm) ||
            result.distance_mm == 0.0f || std::fabs(result.distance_mm) > 100.0f) {
            result.error = BoundedProbeRequestError::InvalidDistance;
            return result;
        }

        const size_t feed_begin = feed_key + strlen(FeedKey);
        if (!parse_finite(request.substr(feed_begin), result.feed_mm_min) || result.feed_mm_min <= 0.0f || result.feed_mm_min > 1000.0f) {
            result.error = BoundedProbeRequestError::InvalidFeed;
            return result;
        }

        result.error = BoundedProbeRequestError::None;
        return result;
    }

    const char* bounded_probe_request_error_message(BoundedProbeRequestError error) {
        switch (error) {
            case BoundedProbeRequestError::None:
                return "ok";
            case BoundedProbeRequestError::Malformed:
                return "expected PROBE,AXIS=X|Z,DISTANCE=mm,FEED=mm/min with no extra fields";
            case BoundedProbeRequestError::InvalidAxis:
                return "AXIS must be X or Z";
            case BoundedProbeRequestError::InvalidDistance:
                return "DISTANCE must be finite, nonzero, and within +/-100 mm";
            case BoundedProbeRequestError::InvalidFeed:
                return "FEED must be finite, positive, and no greater than 1000 mm/min";
        }
        return "invalid probe request";
    }

    void reset_shared_chuck_state() {
        active_shared_chuck_mode = SharedChuckMode::Idle;
        last_program_name.clear();
    }

    bool select_shared_chuck_mode(SharedChuckMode mode) {
        if (!shared_chuck_enabled() || mode == SharedChuckMode::Unavailable) {
            return false;
        }
        active_shared_chuck_mode = mode;
        return true;
    }

    void note_shared_chuck_c_motion() {
        if (shared_chuck_enabled()) {
            active_shared_chuck_mode = SharedChuckMode::CPositioning;
        }
    }

    void note_shared_chuck_cycle_complete() {
        if (active_shared_chuck_mode == SharedChuckMode::CPositioning) {
            active_shared_chuck_mode = SharedChuckMode::Idle;
        }
    }

    void note_shared_chuck_spindle_state(SpindleState state) {
        if (!shared_chuck_enabled()) {
            return;
        }
        if (state == SpindleState::Cw || state == SpindleState::Ccw) {
            active_shared_chuck_mode = SharedChuckMode::Spindle;
        } else if (active_shared_chuck_mode == SharedChuckMode::Spindle) {
            active_shared_chuck_mode = SharedChuckMode::Idle;
        }
    }

    void record_program_name(const std::string& name) {
        if (name.empty()) {
            return;
        }
        constexpr size_t MaxProgramNameLength = 127;
        last_program_name.clear();
        last_program_name.reserve(std::min(name.size(), MaxProgramNameLength));
        for (char c : name) {
            if (last_program_name.size() >= MaxProgramNameLength) {
                break;
            }
            const auto byte = static_cast<unsigned char>(c);
            last_program_name += byte < 0x20 || byte == 0x7f ? '?' : c;
        }
    }

    const std::string& program_name() {
        return last_program_name;
    }

    float css_rpm_from_diameter_mm(float surface_speed, float diameter_mm, bool surface_speed_is_inches_per_minute) {
        return css_rpm_from_diameter_mm(surface_speed, diameter_mm, min_css_diameter_mm(), surface_speed_is_inches_per_minute);
    }

    float css_rpm_from_diameter_mm(float surface_speed, float diameter_mm, float minimum_diameter_mm, bool surface_speed_is_inches_per_minute) {
        if (surface_speed <= 0.0f) {
            return 0.0f;
        }
        const float safe_diameter_mm = std::max(std::fabs(diameter_mm), minimum_diameter_mm);
        const float surface_mm_per_min = surface_speed_is_inches_per_minute ? surface_speed * 25.4f : surface_speed;
        return surface_mm_per_min / (Pi * safe_diameter_mm);
    }

    float clamp_css_rpm(float rpm) {
        return clamp_css_rpm(rpm, max_css_rpm());
    }

    float clamp_css_rpm(float rpm, float max_rpm) {
        if (rpm <= 0.0f) {
            return 0.0f;
        }
        return max_rpm > 0.0f ? std::min(rpm, max_rpm) : rpm;
    }

    float feed_per_rev_to_mm_per_min(float feed_per_rev, float rpm, bool feed_is_inches) {
        const float feed_mm = feed_is_inches ? feed_per_rev * 25.4f : feed_per_rev;
        return feed_mm * std::max(rpm, 0.0f);
    }

    bool feedback_supports_threading(const FeedbackStatus& status) {
        return status.has_measured_rpm && status.has_index_pulse && status.has_angular_position && status.has_direction && !status.stale && !status.fault &&
               status.measured_rpm > 0;
    }

    void EncoderSpindleFeedback::configure(uint32_t pulses_per_revolution, uint32_t stale_timeout_ms) {
        _snapshot_generation.fetch_add(1, std::memory_order_acq_rel);
        _pulses_per_revolution.store(std::max<uint32_t>(pulses_per_revolution, 1), std::memory_order_relaxed);
        _stale_timeout_ms.store(std::max<uint32_t>(stale_timeout_ms, 1), std::memory_order_relaxed);
        _last_pulse_us.store(0, std::memory_order_relaxed);
        _raw_period_us.store(0, std::memory_order_relaxed);
        _filtered_period_us.store(0, std::memory_order_relaxed);
        _pulse_count.store(0, std::memory_order_relaxed);
        _index_pulse_count.store(0, std::memory_order_relaxed);
        _last_index_pulse_count.store(0, std::memory_order_relaxed);
        _last_index_pulses.store(0, std::memory_order_relaxed);
        _signed_position.store(0, std::memory_order_relaxed);
        _last_index_signed_position.store(0, std::memory_order_relaxed);
        _last_index_us.store(0, std::memory_order_relaxed);
        _measured_direction.store(0, std::memory_order_relaxed);
        _commanded_rpm.store(0, std::memory_order_relaxed);
        _timing_trace_head.store(0, std::memory_order_relaxed);
        for (auto& slot : _timing_trace) {
            slot.sequence.store(0, std::memory_order_relaxed);
        }
        _timing_window_start_us = 0;
        _timing_window_end_us = 0;
        _timing_window_period_count = 0;
        _timing_window_min_period_us = 0;
        _timing_window_max_period_us = 0;
        _timing_window_period_sum_us = 0;
        _snapshot_generation.fetch_add(1, std::memory_order_release);
    }

    void EncoderSpindleFeedback::set_commanded_rpm(SpindleSpeed rpm) {
        _commanded_rpm.store(rpm, std::memory_order_relaxed);
    }

    void LATHE_IRAM_ATTR EncoderSpindleFeedback::record_pulse(uint32_t timestamp_us, int8_t direction) {
        _snapshot_generation.fetch_add(1, std::memory_order_acq_rel);
        const uint32_t previous = _last_pulse_us.exchange(timestamp_us, std::memory_order_relaxed);
        const uint32_t period = timestamp_us - previous;
        if (previous != 0 && period != 0) {
            _raw_period_us.store(period, std::memory_order_relaxed);
            record_timing_period(timestamp_us, period);
        }
        _pulse_count.fetch_add(1, std::memory_order_relaxed);
        if (direction != 0) {
            const int8_t normalized = direction > 0 ? 1 : -1;
            _measured_direction.store(normalized, std::memory_order_relaxed);
            _signed_position.fetch_add(normalized, std::memory_order_relaxed);
        }
        _snapshot_generation.fetch_add(1, std::memory_order_release);
    }

    void LATHE_IRAM_ATTR EncoderSpindleFeedback::record_timing_period(uint32_t timestamp_us, uint32_t period_us) {
        if (_timing_window_period_count != 0 && timestamp_us - _timing_window_start_us >= TimingTraceWindowUs) {
            retire_timing_window();
        }
        if (_timing_window_period_count == 0) {
            _timing_window_start_us = timestamp_us - period_us;
            _timing_window_min_period_us = period_us;
            _timing_window_max_period_us = period_us;
            _timing_window_period_sum_us = 0;
        } else {
            if (period_us < _timing_window_min_period_us) _timing_window_min_period_us = period_us;
            if (period_us > _timing_window_max_period_us) _timing_window_max_period_us = period_us;
        }
        _timing_window_end_us = timestamp_us;
        ++_timing_window_period_count;
        _timing_window_period_sum_us += period_us;
    }

    void LATHE_IRAM_ATTR EncoderSpindleFeedback::retire_timing_window() {
        const uint32_t sequence = _timing_trace_head.load(std::memory_order_relaxed) + 1U;
        EncoderTimingSlot& slot = _timing_trace[(sequence - 1U) % TimingTraceWindowCount];
        slot.sequence.store(0, std::memory_order_release);
        slot.start_us = _timing_window_start_us;
        slot.end_us = _timing_window_end_us;
        slot.period_count = _timing_window_period_count;
        slot.min_period_us = _timing_window_min_period_us;
        slot.max_period_us = _timing_window_max_period_us;
        slot.period_sum_us = _timing_window_period_sum_us;
        if (_timing_window_period_count != 0 && _timing_window_period_sum_us != 0) {
            _filtered_period_us.store(
                static_cast<uint32_t>(_timing_window_period_sum_us / _timing_window_period_count),
                std::memory_order_relaxed);
        }
        slot.sequence.store(sequence, std::memory_order_release);
        _timing_trace_head.store(sequence, std::memory_order_release);
        _timing_window_period_count = 0;
    }

    bool EncoderSpindleFeedback::timing_trace_sample(uint32_t sequence, EncoderTimingWindow& sample) const {
        if (sequence == 0) return false;
        const EncoderTimingSlot& slot = _timing_trace[(sequence - 1U) % TimingTraceWindowCount];
        const uint32_t before = slot.sequence.load(std::memory_order_acquire);
        if (before != sequence) return false;
        sample.sequence = before;
        sample.start_us = slot.start_us;
        sample.end_us = slot.end_us;
        sample.period_count = slot.period_count;
        sample.min_period_us = slot.min_period_us;
        sample.max_period_us = slot.max_period_us;
        sample.period_sum_us = slot.period_sum_us;
        return slot.sequence.load(std::memory_order_acquire) == before;
    }

    void LATHE_IRAM_ATTR EncoderSpindleFeedback::record_index(uint32_t timestamp_us) {
        _snapshot_generation.fetch_add(1, std::memory_order_acq_rel);
        const uint32_t pulses = _pulse_count.load(std::memory_order_relaxed);
        const int32_t signed_position = _signed_position.load(std::memory_order_relaxed);
        const uint32_t previous = _last_index_pulse_count.exchange(pulses, std::memory_order_relaxed);
        if (_index_pulse_count.load(std::memory_order_relaxed) != 0) {
            _last_index_pulses.store(pulses - previous, std::memory_order_relaxed);
        }
        _last_index_signed_position.store(signed_position, std::memory_order_relaxed);
        _last_index_us.store(timestamp_us, std::memory_order_relaxed);
        _index_pulse_count.fetch_add(1, std::memory_order_relaxed);
        _snapshot_generation.fetch_add(1, std::memory_order_release);
    }

    FeedbackStatus EncoderSpindleFeedback::status() const {
#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
        return status_at(millis());
#else
        return status_at(_last_pulse_us.load(std::memory_order_relaxed) / 1000U);
#endif
    }

    FeedbackStatus EncoderSpindleFeedback::status_at(uint32_t now_ms) const {
        uint32_t generation_before = 0;
        uint32_t generation_after = 0;
        uint32_t pulses_per_revolution = 1;
        uint32_t stale_timeout_ms = 1;
        uint32_t last_pulse_us = 0;
        uint32_t filtered_period_us = 0;
        uint32_t raw_period_us = 0;
        uint32_t pulse_count = 0;
        uint32_t index_count = 0;
        uint32_t last_index_pulses = 0;
        int32_t signed_position = 0;
        int32_t last_index_signed_position = 0;
        uint32_t last_index_us = 0;
        int8_t measured_direction = 0;
        do {
            generation_before = _snapshot_generation.load(std::memory_order_acquire);
            if (generation_before & 1U) continue;
            pulses_per_revolution = _pulses_per_revolution.load(std::memory_order_relaxed);
            stale_timeout_ms = _stale_timeout_ms.load(std::memory_order_relaxed);
            last_pulse_us = _last_pulse_us.load(std::memory_order_relaxed);
            filtered_period_us = _filtered_period_us.load(std::memory_order_relaxed);
            raw_period_us = _raw_period_us.load(std::memory_order_relaxed);
            pulse_count = _pulse_count.load(std::memory_order_relaxed);
            index_count = _index_pulse_count.load(std::memory_order_relaxed);
            last_index_pulses = _last_index_pulses.load(std::memory_order_relaxed);
            signed_position = _signed_position.load(std::memory_order_relaxed);
            last_index_signed_position = _last_index_signed_position.load(std::memory_order_relaxed);
            last_index_us = _last_index_us.load(std::memory_order_relaxed);
            measured_direction = _measured_direction.load(std::memory_order_relaxed);
            generation_after = _snapshot_generation.load(std::memory_order_acquire);
        } while (generation_before != generation_after || (generation_after & 1U));

        FeedbackStatus status;
        status.commanded_rpm = _commanded_rpm.load(std::memory_order_relaxed);
        status.timestamp_ms  = last_pulse_us / 1000U;
        status.pulse_count = pulse_count;
        status.index_count = index_count;
        status.last_index_pulses = last_index_pulses;
        status.raw_period_us = raw_period_us;
        status.filtered_period_us = filtered_period_us;
        status.timing_trace_head = _timing_trace_head.load(std::memory_order_acquire);
        status.measured_direction = measured_direction;
        status.has_direction = measured_direction != 0;

        const bool has_pulses = last_pulse_us != 0 && pulse_count > 0;
        uint64_t display_period_sum_us = 0;
        uint32_t display_period_count = 0;
        const uint32_t trace_head = status.timing_trace_head;
        for (uint32_t offset = 0; offset < 5U && offset < trace_head; ++offset) {
            EncoderTimingWindow sample;
            if (!timing_trace_sample(trace_head - offset, sample)) break;
            // Do not carry an old stopped/reversed sample into the live RPM.
            if (offset != 0 && sample.end_us != 0 &&
                static_cast<uint32_t>(last_pulse_us - sample.end_us) > 125000U) break;
            display_period_sum_us += sample.period_sum_us;
            display_period_count += sample.period_count;
        }
        if (has_pulses && display_period_count != 0 && display_period_sum_us != 0) {
            status.measured_rpm = (60.0f * 1000000.0f * static_cast<float>(display_period_count)) /
                                  (static_cast<float>(display_period_sum_us) * static_cast<float>(pulses_per_revolution));
            status.has_measured_rpm = true;
        } else if (has_pulses && filtered_period_us != 0) {
            status.measured_rpm = (60.0f * 1000000.0f) /
                                  (static_cast<float>(filtered_period_us) * static_cast<float>(pulses_per_revolution));
            status.has_measured_rpm = true;
        }

        const bool has_indexed_angle = index_count > 0 && last_index_us != 0;
        const int32_t phase_origin = has_indexed_angle ? last_index_signed_position : 0;
        const int32_t phase = (((signed_position - phase_origin) % static_cast<int32_t>(pulses_per_revolution)) +
                               static_cast<int32_t>(pulses_per_revolution)) %
                              static_cast<int32_t>(pulses_per_revolution);
        status.has_index_pulse       = index_count > 0;
        status.has_angular_position  = has_pulses;
        status.has_indexed_angle     = has_indexed_angle;
        status.revolution_count      = pulse_count / pulses_per_revolution;
        status.angular_position_rev  = static_cast<float>(phase) / static_cast<float>(pulses_per_revolution);
        status.last_pulse_age_ms     = has_pulses ? now_ms - status.timestamp_ms : 0;
        status.stale                 = !has_pulses ||
                                       (static_cast<int32_t>(status.last_pulse_age_ms) > 0 && status.last_pulse_age_ms > stale_timeout_ms);
        // Index is observational until separately commissioned. Missing or
        // malformed index pulses must not invalidate proven A/B feedback.
        status.fault                 = false;
        return status;
    }

    bool EncoderSpindleFeedback::synchronize_for_threading_start() const {
        return feedback_supports_threading(status());
    }

    float x_offset_to_machine_mm(float x_offset, DiameterMode mode) {
        return mode == DiameterMode::Diameter ? x_offset * 0.5f : x_offset;
    }

    float x_program_to_machine_mm(float x_programmed, DiameterMode mode) {
        return x_offset_to_machine_mm(x_programmed, mode);
    }

    float x_machine_to_diameter_mm(float x_machine) {
        return std::fabs(x_machine) * 2.0f;
    }

    bool load_tool_table() {
        reset_tool_table();
        tool_table_loaded = true;

        size_t len = sizeof(tool_table);
        if (nvs.get_blob(ToolTableNvsKey, tool_table.data(), &len) || len != sizeof(tool_table)) {
            reset_tool_table();
            return false;
        }
        return true;
    }

    bool save_tool_table() {
        ensure_tool_table_loaded();
        if (FORCE_BUFFER_SYNC_DURING_NVS_WRITE) {
            protocol_buffer_synchronize();
        }
        return !nvs.set_blob(ToolTableNvsKey, tool_table.data(), sizeof(tool_table));
    }

    void clear_tool_table(bool persist) {
        reset_tool_table();
        tool_table_loaded = true;
        if (persist) {
            if (FORCE_BUFFER_SYNC_DURING_NVS_WRITE) {
                protocol_buffer_synchronize();
            }
            nvs.erase_key(ToolTableNvsKey);
        }
    }

    void set_tool_data(uint32_t tool_number, const ToolData& data) {
        ensure_tool_table_loaded();
        ToolData stored = data;
        stored.valid    = true;

        for (auto& slot : tool_table) {
            if (slot.data.valid && slot.tool_number == tool_number) {
                slot.data = stored;
                save_tool_table();
                return;
            }
        }

        for (auto& slot : tool_table) {
            if (!slot.data.valid) {
                slot.tool_number = tool_number;
                slot.data        = stored;
                save_tool_table();
                return;
            }
        }

        // Fixed-size first release: replace the last slot rather than allocating dynamically.
        tool_table.back().tool_number = tool_number;
        tool_table.back().data        = stored;
        save_tool_table();
    }

    std::optional<ToolData> get_tool_data(uint32_t tool_number) {
        ensure_tool_table_loaded();
        for (const auto& slot : tool_table) {
            if (slot.data.valid && slot.tool_number == tool_number) {
                return slot.data;
            }
        }
        return std::nullopt;
    }

    ActiveToolOffset select_tool(uint32_t tool_number) {
        ensure_tool_table_loaded();
        active_offset = {};
        active_offset.tool_number = tool_number;

        auto tool = get_tool_data(tool_number);
        if (!tool) {
            return active_offset;
        }

        active_offset.x_mm           = tool->geometry_x_mm + tool->wear_x_mm;
        active_offset.z_mm           = tool->geometry_z_mm + tool->wear_z_mm;
        active_offset.nose_radius_mm = tool->nose_radius_mm;
        active_offset.orientation    = tool->orientation;
        active_offset.valid          = true;
        return active_offset;
    }

    ActiveToolOffset active_tool_offset() {
        return active_offset;
    }

    Error touch_off_tool(const TouchOffSpec& spec) {
        if (spec.tool_number == 0 || (!spec.set_x && !spec.set_z)) {
            return Error::GcodeValueWordInvalid;
        }
        if ((spec.set_x && (!std::isfinite(spec.machine_x_mm) || !std::isfinite(spec.reference_x_mm))) ||
            (spec.set_z && (!std::isfinite(spec.machine_z_mm) || !std::isfinite(spec.reference_z_mm)))) {
            return Error::GcodeValueWordInvalid;
        }

        ToolData data;
        if (auto existing = get_tool_data(spec.tool_number)) {
            data = *existing;
        }

        if (spec.set_x) {
            const float reference_machine_x = x_program_to_machine_mm(spec.reference_x_mm, spec.x_mode);
            data.geometry_x_mm = reference_machine_x - spec.machine_x_mm - data.wear_x_mm;
        }
        if (spec.set_z) {
            data.geometry_z_mm = spec.reference_z_mm - spec.machine_z_mm - data.wear_z_mm;
        }

        set_tool_data(spec.tool_number, data);
        if (active_offset.tool_number == spec.tool_number) {
            select_tool(spec.tool_number);
        }
        return Error::Ok;
    }

    namespace {
        bool append_cycle_move(CyclePlan& plan, CycleMoveKind kind, float x_mm, float z_mm, float feed) {
            if (plan.count >= MaxCycleMoves) {
                plan.error = Error::Overflow;
                plan.valid = false;
                return false;
            }
            plan.moves[plan.count++] = CycleMove { kind, x_mm, z_mm, feed };
            return true;
        }
    }

    CyclePlan build_threading_cycle(const ThreadingCycleSpec& spec) {
        CyclePlan plan;
        if (spec.passes == 0 || spec.passes > MaxCycleMoves || spec.pitch_mm <= 0.0f || spec.start_z_mm == spec.end_z_mm ||
            spec.start_x_mm == spec.end_x_mm) {
            plan.error = Error::GcodeValueWordInvalid;
            return plan;
        }

        for (uint8_t pass = 1; pass <= spec.passes; ++pass) {
            const float blend = static_cast<float>(pass) / static_cast<float>(spec.passes);
            const float x_mm  = spec.start_x_mm + ((spec.end_x_mm - spec.start_x_mm) * blend);
            if (!append_cycle_move(plan, CycleMoveKind::Threading, x_mm, spec.end_z_mm, spec.pitch_mm)) {
                return plan;
            }
            if (pass != spec.passes && !append_cycle_move(plan, CycleMoveKind::Rapid, spec.start_x_mm, spec.start_z_mm, 0.0f)) {
                return plan;
            }
        }

        plan.valid = true;
        return plan;
    }

    CyclePlan build_rough_turning_cycle(const RoughTurningCycleSpec& spec) {
        CyclePlan plan;
        if (spec.depth_step_mm <= 0.0f || spec.rough_feed_mm_min <= 0.0f || spec.start_z_mm == spec.end_z_mm ||
            spec.start_x_mm == spec.final_x_mm) {
            plan.error = Error::GcodeValueWordInvalid;
            return plan;
        }

        const float direction = spec.final_x_mm > spec.start_x_mm ? 1.0f : -1.0f;
        float x_mm = spec.start_x_mm;
        while ((direction > 0.0f && x_mm < spec.final_x_mm) || (direction < 0.0f && x_mm > spec.final_x_mm)) {
            x_mm += direction * spec.depth_step_mm;
            if ((direction > 0.0f && x_mm > spec.final_x_mm) || (direction < 0.0f && x_mm < spec.final_x_mm)) {
                x_mm = spec.final_x_mm;
            }
            if (!append_cycle_move(plan, CycleMoveKind::Linear, x_mm, spec.end_z_mm, spec.rough_feed_mm_min)) {
                return plan;
            }
            if (x_mm != spec.final_x_mm && !append_cycle_move(plan, CycleMoveKind::Rapid, spec.start_x_mm, spec.start_z_mm, 0.0f)) {
                return plan;
            }
        }

        if (spec.include_finish_pass && !append_cycle_move(plan, CycleMoveKind::Linear, spec.final_x_mm, spec.end_z_mm, spec.rough_feed_mm_min)) {
            return plan;
        }

        plan.valid = plan.count > 0;
        return plan;
    }

    CyclePlan build_finishing_cycle(const FinishingCycleSpec& spec) {
        CyclePlan plan;
        if (spec.feed_mm_min <= 0.0f || (spec.start_x_mm == spec.end_x_mm && spec.start_z_mm == spec.end_z_mm)) {
            plan.error = Error::GcodeValueWordInvalid;
            return plan;
        }

        if (!append_cycle_move(plan, CycleMoveKind::Linear, spec.end_x_mm, spec.end_z_mm, spec.feed_mm_min)) {
            return plan;
        }
        plan.valid = true;
        return plan;
    }

    CyclePlan build_grooving_cycle(const GroovingCycleSpec& spec) {
        CyclePlan plan;
        if (spec.peck_depth_mm <= 0.0f || spec.feed_mm_min <= 0.0f || spec.start_x_mm == spec.final_x_mm) {
            plan.error = Error::GcodeValueWordInvalid;
            return plan;
        }

        const float direction = spec.final_x_mm > spec.start_x_mm ? 1.0f : -1.0f;
        float x_mm = spec.start_x_mm;
        while ((direction > 0.0f && x_mm < spec.final_x_mm) || (direction < 0.0f && x_mm > spec.final_x_mm)) {
            x_mm += direction * spec.peck_depth_mm;
            if ((direction > 0.0f && x_mm > spec.final_x_mm) || (direction < 0.0f && x_mm < spec.final_x_mm)) {
                x_mm = spec.final_x_mm;
            }
            if (!append_cycle_move(plan, CycleMoveKind::Linear, x_mm, spec.z_mm, spec.feed_mm_min)) {
                return plan;
            }
            if (x_mm != spec.final_x_mm && !append_cycle_move(plan, CycleMoveKind::Rapid, spec.start_x_mm, spec.z_mm, 0.0f)) {
                return plan;
            }
        }

        plan.valid = plan.count > 0;
        return plan;
    }

    CyclePlan build_peck_drilling_cycle(const PeckDrillingCycleSpec& spec) {
        CyclePlan plan;
        if (spec.peck_depth_mm <= 0.0f || spec.feed_mm_min <= 0.0f || spec.start_z_mm == spec.final_z_mm) {
            plan.error = Error::GcodeValueWordInvalid;
            return plan;
        }

        const float direction = spec.final_z_mm > spec.start_z_mm ? 1.0f : -1.0f;
        float z_mm = spec.start_z_mm;
        while ((direction > 0.0f && z_mm < spec.final_z_mm) || (direction < 0.0f && z_mm > spec.final_z_mm)) {
            z_mm += direction * spec.peck_depth_mm;
            if ((direction > 0.0f && z_mm > spec.final_z_mm) || (direction < 0.0f && z_mm < spec.final_z_mm)) {
                z_mm = spec.final_z_mm;
            }
            if (!append_cycle_move(plan, CycleMoveKind::Linear, spec.x_mm, z_mm, spec.feed_mm_min)) {
                return plan;
            }
            if (z_mm != spec.final_z_mm && !append_cycle_move(plan, CycleMoveKind::Rapid, spec.x_mm, spec.start_z_mm, 0.0f)) {
                return plan;
            }
        }

        plan.valid = plan.count > 0;
        return plan;
    }

    float spindle_revolutions(const FeedbackStatus& status) {
        return static_cast<float>(status.revolution_count) + status.angular_position_rev;
    }

    float synchronized_thread_z(const ThreadingSyncState& state, float spindle_revolutions) {
        if (state.pitch_mm <= 0.0f || state.start_z_mm == state.end_z_mm) {
            return state.start_z_mm;
        }

        const float direction = state.end_z_mm > state.start_z_mm ? 1.0f : -1.0f;
        const float travel = (spindle_revolutions - state.start_spindle_revolutions) * state.pitch_mm * direction;
        const float target = state.start_z_mm + travel;

        if (direction > 0.0f) {
            return std::min(target, state.end_z_mm);
        }
        return std::max(target, state.end_z_mm);
    }

    float synchronized_thread_progress(const ThreadingSyncState& state, float spindle_revolutions) {
        const float z_travel = state.end_z_mm - state.start_z_mm;
        if (z_travel == 0.0f || state.pitch_mm <= 0.0f) {
            return 1.0f;
        }
        const float z = synchronized_thread_z(state, spindle_revolutions);
        return std::min(std::max((z - state.start_z_mm) / z_travel, 0.0f), 1.0f);
    }

    Error validate_feature(Feature feature) {
        if (!enabled()) {
            log_info("Lathe " << feature_name(feature) << " requires the machine/lathe configuration section to be enabled");
            return Error::GcodeUnsupportedCommand;
        }
        if (!feature_enabled(feature)) {
            log_info("Lathe " << feature_name(feature) << " is disabled in configuration");
            return Error::GcodeUnsupportedCommand;
        }

        return Error::Ok;
    }
}
