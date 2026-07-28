#pragma once

#include <cstddef>
#include <cstdint>

// Alarm codes.
enum class ExecAlarm : uint8_t {
    None                  = 0,
    HardLimit             = 1,
    SoftLimit             = 2,
    AbortCycle            = 3,
    ProbeFailInitial      = 4,
    ProbeFailContact      = 5,
    HomingFailReset       = 6,
    HomingFailDoor        = 7,
    HomingFailPulloff     = 8,
    HomingFailApproach    = 9,
    SpindleControl        = 10,
    StartupPin            = 11,  // control or limit input pin active
    HomingAmbiguousSwitch = 12,
    HardStop              = 13,
    Unhomed               = 14,
    Init                  = 15,
    ExpanderReset         = 16,
    GCodeError            = 17,
    ProbeHardLimit        = 18,
    LatheSync             = 19,
};

extern volatile ExecAlarm lastAlarm;

#ifdef TAMS_MAIJKER_ALARM_ASSETS
// Bounded, read-only controller alarm history used by the Maijker TAMS
// telemetry snapshot. Entries are recorded in protocol task context (never
// from an ISR) and are ordered by their monotonically increasing sequence.
struct AlarmTelemetryRecord {
    uint32_t  sequence           = 0;
    uint32_t  occurred_uptime_ms = 0;
    ExecAlarm code               = ExecAlarm::None;
};

constexpr size_t AlarmTelemetryCapacity = 16;

size_t      copy_alarm_telemetry(AlarmTelemetryRecord* destination, size_t capacity);
const char* alarm_native_code(ExecAlarm alarm);
const char* alarm_source(ExecAlarm alarm);
const char* alarm_native_severity(ExecAlarm alarm);
#endif
