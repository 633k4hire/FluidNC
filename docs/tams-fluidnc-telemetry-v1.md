# TAMS FluidNC telemetry and bounded-control contract

This document is the wire contract between the Maijker XZACT FluidNC firmware,
`MTConnect.FluidNcAdapter`, and the TAMS LVGL lathe HMI. It deliberately does
not expose an arbitrary G-code or settings write API.

## Transport and authentication

The commands use FluidNC's normal command channel:

- `$ESP425` is a read command and emits one compact JSON object on one physical
  line.
- `$ESP426=...` and `$ESP427=...` are administrator-authenticated writes.
- The normal FluidNC channel acknowledgement may follow the JSON response.
- Consumers must select a response by its `schema` or `cmd`, not by assuming it
  is the only line received.

The firmware caps an `ESP425` snapshot at 8192 bytes and removes CR/LF from the
payload. A snapshot that exceeds the cap fails closed with:

```json
{"schema":"tams.fluidnc.telemetry.v1","error":"snapshot_too_large"}
```

## `$ESP425` read-only snapshot

The schema identifier is `tams.fluidnc.telemetry.v1`.

### Field and unit rules

- `sequence` is monotonically increasing for the current boot.
- `uptime_ms` is a monotonic 64-bit millisecond count that extends across the
  ESP32 `millis()` wrap.
- X and Z `machine`, `work`, and `work_offset` values are always millimeters.
- C `machine`, `work`, and `work_offset` values are always degrees.
- `positions.*.native_units` is therefore `MILLIMETER` for X/Z and `DEGREE`
  for C.
- `execution.programmed_feed_units` is one of
  `MILLIMETER_PER_MINUTE`, `INCH_PER_MINUTE`,
  `MILLIMETER_PER_REVOLUTION`, `INCH_PER_REVOLUTION`, or `PER_MINUTE`
  for G93 inverse-time feed.
- `execution.realtime_feed_mm_per_min` is always millimeters per minute.
- Spindle speed fields are revolutions per minute.
- `spindle.encoder.angular_position_revolution` is a unitless value in
  `[0,1)`, not degrees.
- Tool geometry, wear, combined offsets, nose radius, and probe X/Z positions
  are millimeters. Probe C position is degrees.
- `tool.geometry_*_mm` and `tool.wear_*_mm` are the separately stored values.
  `tool.x_offset_mm` and `tool.z_offset_mm` are their combined active offsets.
  The four separate values are `null` when no valid active tool exists.
- Override values are integer percentages.
- JSON `null`, not zero, represents unavailable line, measured-RPM, or angular
  position data.

### Exact object shape

```text
schema
sequence
uptime_ms
machine
  manufacturer
  model
  configured_name
  board
  firmware
  state
  availability
  alarm_code
  alarm
  alarm_active
  alarm_native_code
  alarm_source
  alarm_native_severity
alarm_history[] (oldest to newest, maximum 16 records per boot)
  sequence
  occurred_uptime_ms
  code
  native_code
  source
  native_severity
  text
  active
execution
  state
  controller_mode
  motion_mode
  feed_mode
  units
  distance_mode
  plane
  coordinate_system
  programmed_feed
  programmed_feed_units
  realtime_feed_mm_per_min
  program
    name
    active
    source
  line
    gcode_n
    source
    provenance
positions
  x|z|c
    available
    native_units
    machine
    work
    work_offset
    homed
    limit_active
spindle
  shared_chuck
  mode
  c_axis
  state
  programmed_s
  commanded_rpm
  measured_rpm
  speed_mode
  diameter_mode
  encoder
    configured
    capture_active
    pulses_per_revolution
    has_measured_rpm
    has_index
    has_angular_position
    angular_position_revolution
    revolution_count
    stale
    fault
tool
  selected
  current
  offset_available
  geometry_x_mm
  geometry_z_mm
  wear_x_mm
  wear_z_mm
  x_offset_mm
  z_offset_mm
  nose_radius_mm
  orientation
assets
  cutting_tools[] (configured tools only; empty stations are omitted)
    asset_id
    tool_id
    serial_number
    station
    active
    status
    geometry_x_mm
    geometry_z_mm
    wear_x_mm
    wear_z_mm
    nose_radius_mm
    orientation
turret
  configured
  station_count
  current_station
  target_station
  software_position_known
  sensor_configured
  sensor_active
  mechanically_confirmed
  position_basis
  last_error
probe
  configured
  active
  cycle_active
  last_succeeded
  last_machine_position
    x
    z
    c
overrides
  feed_percent
  rapid_percent
  spindle_percent
coolant
  flood_available
  mist_available
  flood_active
  mist_active
capabilities
  read_only_snapshot
  raw_remote_write
  emergency_stop_feedback
  emergency_stop_active
  turret_index_feedback
  feed_hold
  jog_cancel
  spindle_stop
  reset_abort
  automatic_resume_after_reconnect
conditions[]
  level
  source
  code
  text
```

### Alarm and asset semantics

Each native `ExecAlarm` has a stable `FLUIDNC_ALARM_nn` code, a bounded
source (`LIMIT`, `PROBE`, `MOTION`, `SPINDLE`, `PROGRAM`, or `CONTROLLER`),
and native severity (`FAULT` or `CRITICAL`). Alarm records are captured in
normal protocol-task context, never from an interrupt, and retained in a
fixed 16-entry RAM ring for the current boot. Clearing an alarm sets
`machine.alarm_active` false without erasing the ring, allowing the adapter to
publish the corresponding MTConnect `NORMAL` transition and retain bounded
diagnostic history. A reboot is evident from `uptime_ms` and resets the ring.

`assets.cutting_tools` includes only tool records actually stored by the
controller. It never invents assets for empty turret stations. The stable
controller-side asset identifier is used as `serial_number` because this
firmware has no manufacturer-marked tool serial input; consumers must not
present it as a physical manufacturer serial.

### Representative snapshot

The example is formatted for review. Firmware sends the same object on one
physical line.

```json
{
  "schema": "tams.fluidnc.telemetry.v1",
  "sequence": 17,
  "uptime_ms": 42857,
  "machine": {
    "manufacturer": "TAMS",
    "model": "Maijker XZACT Mini Lathe",
    "configured_name": "XZACt_MiniLathe",
    "board": "MKS-DLC32 V2.1",
    "firmware": "FluidNC v3.9.x",
    "state": "Cycle",
    "availability": "AVAILABLE",
    "alarm_code": 0,
    "alarm": "None",
    "alarm_active": false,
    "alarm_native_code": "FLUIDNC_ALARM_00",
    "alarm_source": "CONTROLLER",
    "alarm_native_severity": "NORMAL"
  },
  "alarm_history": [],
  "execution": {
    "state": "ACTIVE",
    "controller_mode": "AUTOMATIC",
    "motion_mode": "G1_LINEAR",
    "feed_mode": "UNITS_PER_REVOLUTION",
    "units": "MILLIMETER",
    "distance_mode": "ABSOLUTE",
    "plane": "ZX",
    "coordinate_system": "G54",
    "programmed_feed": 0.15,
    "programmed_feed_units": "MILLIMETER_PER_REVOLUTION",
    "realtime_feed_mm_per_min": 180.0,
    "program": {
      "name": "lathe-part.nc",
      "active": true,
      "source": "FILE"
    },
    "line": {
      "gcode_n": 120,
      "source": 42,
      "provenance": "PLANNER_EXECUTING"
    }
  },
  "positions": {
    "x": {
      "available": true,
      "native_units": "MILLIMETER",
      "machine": 24.5,
      "work": 4.5,
      "work_offset": 20.0,
      "homed": true,
      "limit_active": false
    },
    "z": {
      "available": true,
      "native_units": "MILLIMETER",
      "machine": -31.25,
      "work": -1.25,
      "work_offset": -30.0,
      "homed": true,
      "limit_active": false
    },
    "c": {
      "available": true,
      "native_units": "DEGREE",
      "machine": 90.0,
      "work": 90.0,
      "work_offset": 0.0,
      "homed": false,
      "limit_active": false
    }
  },
  "spindle": {
    "shared_chuck": true,
    "mode": "SPINDLE",
    "c_axis": "C",
    "state": "CLOCKWISE",
    "programmed_s": 1200.0,
    "commanded_rpm": 1200.0,
    "measured_rpm": 1198.5,
    "speed_mode": "FIXED_RPM",
    "diameter_mode": "DIAMETER",
    "encoder": {
      "configured": true,
      "capture_active": true,
      "pulses_per_revolution": 64,
      "has_measured_rpm": true,
      "has_index": true,
      "has_angular_position": true,
      "angular_position_revolution": 0.375,
      "revolution_count": 816,
      "stale": false,
      "fault": false
    }
  },
  "tool": {
    "selected": 2,
    "current": 2,
    "offset_available": true,
    "geometry_x_mm": 0.1,
    "geometry_z_mm": -0.04,
    "wear_x_mm": 0.025,
    "wear_z_mm": -0.01,
    "x_offset_mm": 0.125,
    "z_offset_mm": -0.05,
    "nose_radius_mm": 0.4,
    "orientation": 3
  },
  "assets": {
    "cutting_tools": []
  },
  "turret": {
    "configured": true,
    "station_count": 5,
    "current_station": 2,
    "target_station": 2,
    "software_position_known": true,
    "sensor_configured": false,
    "sensor_active": false,
    "mechanically_confirmed": false,
    "position_basis": "software_dead_reckoning",
    "last_error": "ok"
  },
  "probe": {
    "configured": true,
    "active": false,
    "cycle_active": false,
    "last_succeeded": true,
    "last_machine_position": {
      "x": 23.1,
      "z": -31.25,
      "c": 90.0
    }
  },
  "overrides": {
    "feed_percent": 100,
    "rapid_percent": 100,
    "spindle_percent": 100
  },
  "coolant": {
    "flood_available": false,
    "mist_available": false,
    "flood_active": false,
    "mist_active": false
  },
  "capabilities": {
    "read_only_snapshot": true,
    "raw_remote_write": false,
    "emergency_stop_feedback": false,
    "emergency_stop_active": false,
    "turret_index_feedback": false,
    "feed_hold": true,
    "jog_cancel": true,
    "spindle_stop": true,
    "reset_abort": true,
    "automatic_resume_after_reconnect": false
  },
  "conditions": [
    {
      "level": "UNAVAILABLE",
      "source": "SAFETY",
      "code": "ESTOP_FEEDBACK_UNAVAILABLE",
      "text": "Physical E-stop removes power but has no controller feedback input"
    },
    {
      "level": "UNAVAILABLE",
      "source": "TURRET",
      "code": "TURRET_POSITION_FEEDBACK_UNAVAILABLE",
      "text": "Turret station is software dead-reckoned and not mechanically confirmed"
    }
  ]
}
```

### Execution provenance

`execution.line.provenance` has two values:

- `PLANNER_EXECUTING`: line data came from the planner block currently being
  executed. `source` is the physical one-based line in the source file/channel
  that produced that block, while `gcode_n` is the optional G-code `N` word.
- `PARSER_LAST`: no planner block is executing, so `gcode_n` is the last
  parser line number and `source` is `null`.

`execution.program.name` is bounded to 127 characters and has control
characters replaced. It remains available as the last program name after a job
finishes; `execution.program.active` is authoritative for whether a file is
currently active.

## `$ESP426` shared-chuck ownership

The C stepper drive and HBridge spindle drive turn the same physical chuck.
Their ownership must be mutually exclusive.

The only valid requests are:

```text
$ESP426=MODE=IDLE
$ESP426=MODE=C_POSITIONING
$ESP426=MODE=SPINDLE
```

The command is accepted only when the controller and planner are idle and the
spindle output is stopped. It selects ownership but never energizes an output
or starts motion. Malformed parameters, extra fields, and unsupported modes
are rejected.

Successful response:

```json
{"cmd":"426","status":"ok","mode":"C_POSITIONING","message":"ownership selected; outputs remain stopped"}
```

Normal G-code receives the same protection. A block that requests spindle
output and C-axis motion together is rejected. An M5/C block may stop the
spindle and transfer ownership in that order. A C-to-spindle transition
synchronizes the planner before the HBridge can start.

## `$ESP427` bounded probe

The exact request grammar is:

```text
$ESP427=PROBE,AXIS=X|Z,DISTANCE=<signed-finite-mm>,FEED=<positive-finite-mm-per-minute>
```

Constraints:

- request length is at most 96 bytes;
- only X or Z is accepted;
- distance must be nonzero and its absolute value must not exceed 100 mm;
- feed must be greater than zero and at most 1000 mm/min;
- no whitespace, reordered keys, suffixes, or extra fields are accepted;
- the controller and planner must be idle;
- the configured probe input must exist;
- the spindle must be stopped and shared-chuck ownership must be `IDLE`.

The command performs one no-error probe move without changing the persistent
modal state. Contact is explicit in the response:

```json
{"cmd":"427","status":"ok","axis":"X","contact":true,"position_mm":23.100,"message":"probe contact detected"}
```

No contact returns an error status and the final reached position. The caller
must never infer contact merely because motion stopped.

## Stop and safety semantics

The adapter may expose only the following existing FluidNC real-time stop
actions:

- feed hold (`!`);
- jog cancel (`0x85`);
- spindle stop override (`0x9E`);
- reset/abort (`Ctrl-X`, `0x18`).

These are software controls, not an emergency stop. This machine's physical
E-stop removes actuator power and has no controller feedback input. Therefore:

- `emergency_stop_feedback` and `emergency_stop_active` remain false;
- the snapshot always reports `ESTOP_FEEDBACK_UNAVAILABLE`;
- the adapter and HMI must label software stop/reset actions truthfully;
- neither component may claim that the physical E-stop is active or healthy.

The turret likewise has no index-confirmation sensor. Its station is software
dead-reckoned, `mechanically_confirmed` remains false, and the unavailable
condition remains present.

## Build target

The telemetry firmware uses the machine-specific OTA-safe build environment:

```powershell
$env:PLATFORMIO_CORE_DIR = 'C:\repos\FluidNC\.pio-core'
pio run -e maijker_wifi
```

`maijker_wifi` omits the unused onboard-OLED implementation because the
machine uses the external TAMS LVGL HMI. That keeps the firmware within the
standard 4 MiB `min_littlefs.csv` layout with two equal OTA application slots
and enough LittleFS space for the bundled WebUI/configuration files. The
repository pins PlatformIO `espressif32` 6.9.0 so builds do not silently move
to an incompatible framework/toolchain family.
