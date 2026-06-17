# FluidNC lathe configuration and operator guide

This document describes the lathe configuration scaffold added by the lathe rollout. The feature is intentionally gated: existing mill/router/laser/plasma configurations remain unchanged unless a `lathe:` block is present and `enable: true`.

## Minimal lathe configuration

Add the `lathe:` block at the top level of the machine YAML:

```yaml
lathe:
  enable: true
  enable_css: true
  enable_feed_per_rev: true
  enable_threading: false   # keep false until an encoder feedback backend is configured
  min_css_diameter_mm: 1.0
  max_css_rpm: 2500
  x_axis: 0                 # X axis index; 0=X, 1=Y, 2=Z, ...
  z_axis: 2                 # Z axis index
  feedback_stale_ms: 250
  encoder_enable: true
  encoder_pulse_pin: gpio.34
  encoder_index_pin: gpio.35
  encoder_pulses_per_rev: 1024
```

### Required safety rules

- `enable_css`, `enable_feed_per_rev`, and `enable_threading` require `enable: true`.
- CSS requires `max_css_rpm` greater than zero so `G96` cannot accelerate the spindle without a clamp.
- CSS requires `min_css_diameter_mm` greater than zero to avoid infinite RPM near centerline.
- `x_axis` and `z_axis` must be different.
- Threading should remain disabled until spindle feedback reports measured RPM, index pulse, angular position, non-stale state, and no fault.
- `encoder_enable: true` requires `enable: true`, a valid `encoder_pulse_pin`, and `encoder_pulses_per_rev` greater than zero.
- Threading with the built-in encoder path requires an `encoder_index_pin` so each synchronized pass can align to a known spindle revolution.

## Supported lathe modal commands

| Command | Purpose | Notes |
| --- | --- | --- |
| `G95` | Feed per revolution | Converts feed using measured RPM when available, otherwise effective commanded RPM. |
| `G96` | Constant surface speed | `S` is treated as surface speed and is clamped by `max_css_rpm`. |
| `G97` | Fixed RPM | `S` is treated as spindle RPM. |
| `G32`/`G33` | Guarded threading primitive | Requires `G95`, active spindle, and encoder-capable feedback. |
| `G7` | Diameter programming | X words are interpreted as diameter values and converted to internal machine-radius coordinates before planning. |
| `G8` | Radius programming | X words are interpreted directly as machine-radius coordinates. |

## Diameter and radius programming

FluidNC stores and plans the configured lathe X axis as a machine-radius coordinate. `G8` leaves X words unchanged. `G7` accepts operator/CAM diameter values, converts the programmed X word to radius after inch/mm conversion, and then applies the normal coordinate-system, offset, and planner pipeline.

Examples:

```gcode
G21 G8
G1 X12.000 Z-5.000 F100   ; X is 12 mm radius

G21 G7
G1 X24.000 Z-5.000 F100   ; X is 24 mm diameter, planned as 12 mm radius
```

CSS calculations use the inverse conversion: the internal machine-radius X coordinate is doubled back to cutting diameter before `G96` RPM math is applied. This keeps `G96` behavior consistent whether a program is written in `G7` or `G8`.

## Tool offsets and persistence

Lathe tool data is a fixed-size table persisted in ESP32 NVS under the `LatheTools` blob key. Each entry stores:

- X/Z geometry offsets;
- X/Z wear offsets;
- tool nose radius;
- insert orientation.

The active X/Z lathe offset is applied through the existing TLO vector on `M6` and `M61` while lathe mode is enabled. Tool geometry and wear are summed only when a tool becomes active. X offsets are stored as machine-radius millimeters; diameter-mode touch-off workflows must convert diameter values to radius before storage.

Firmware APIs now load the table lazily from NVS, save it whenever `Lathe::set_tool_data()` changes a slot, and provide `Lathe::clear_tool_table(true)` for clearing the persisted table. The first-class operator UI still needs an editing/touch-off screen, but the firmware storage layer no longer loses tool data on reboot.

### Touch-off workflow API

The firmware layer now includes `Lathe::touch_off_tool()` for operator touch-off workflows. A caller supplies the tool number, current machine X/Z position at the touch point, the known reference X/Z value, and whether X is being entered in `G7` diameter mode or `G8` radius mode. The helper:

- converts diameter-mode X references to machine-radius coordinates;
- calculates geometry offsets as `reference - machine_position - wear`;
- preserves existing wear, nose radius, and insert orientation data;
- persists the updated tool table through `Lathe::set_tool_data()`;
- refreshes the active lathe offset if the touched-off tool is already selected.

This is the firmware-side foundation for a WebUI touch-off panel. The remaining UI work is to collect the current machine position/reference values from the operator, call the firmware command layer that wraps this helper, and refresh `ESP421` lathe status after saving.

## WebUI/API support

The WebUI command endpoint exposes lathe status via:

```text
/command?cmd=%5BESP421%5D
```

The JSON response contains `cmd: "421"`, `status: "ok"`, and a `data` array of id/value pairs for:

- lathe enable state;
- spindle speed mode (`G96`/`G97`);
- diameter/radius mode;
- feed mode (`G93`/`G94`/`G95`);
- programmed `S` and effective RPM;
- CSS clamp and minimum CSS diameter;
- encoder enable state and pulses per revolution;
- active lathe tool offsets and nose radius;
- spindle feedback measured RPM/index/angular/stale/fault state.

The fallback embedded WebUI tool page includes a Lathe Status panel that calls `ESP421`. The main WebUI should use the same endpoint so it can display lathe state without parsing free-form text.

Additional firmware WebUI command endpoints are available for tool workflows:

```text
/command?cmd=%5BESP422%5DT=7%20GX=1.0%20GZ=2.0%20WX=0.0%20WZ=0.0%20NR=0.4%20O=1
/command?cmd=%5BESP423%5DT=7%20MX=10.0%20RX=24.0%20MODE=diameter%20MZ=-4.0%20RZ=0.0
```

- `ESP422` saves a lathe tool table entry. Parameters are `T` for tool number, `GX/GZ` for geometry, `WX/WZ` for wear, `NR` for nose radius, and `O` for insert orientation.
- `ESP423` performs a touch-off calculation. X touch-off uses `MX/RX` and optional `MODE=diameter`; Z touch-off uses `MZ/RZ`. Supplying only X or only Z updates only that axis.
- Both commands return JSON using the standard `cmd`, `status`, and `data` fields, then the UI should refresh `ESP421`.

## Encoder feedback and synchronized threading foundation

The firmware now includes an `EncoderSpindleFeedback` backend core that can consume encoder pulse/index timestamps and report measured RPM, angular position, index availability, stale state, and fault-ready status through the same `SpindleFeedback` contract used by `G95`, CSS, and threading guards.

The synchronized threading math layer also includes a spindle-revolution-to-Z-position helper. It maps spindle revolutions to commanded Z position using thread pitch and clamps to the programmed thread endpoint. This provides the deterministic trajectory primitive needed by the real-time step generator.

Remaining hardware integration work:

- bind configured ESP32-S3 GPIO/PCNT/RMT/interrupt inputs to `EncoderSpindleFeedback::record_pulse()` and `record_index()`;
- decide whether each spindle driver owns its encoder instance or whether lathe mode owns a shared encoder instance;
- move the synchronized trajectory primitive into the step-generation path so thread Z motion is advanced from encoder phase rather than planner time;
- validate behavior with low-speed air cuts before cutting material.

### Blazor/WebUI implementation note

If a future .NET 8 Blazor WebUI consumes the lathe endpoint, do not put C# lambdas inside quoted Razor attributes such as `@bind-value:get="() => ..."` on native HTML elements. Use .NET 8 binding modifiers (`@bind`, `@bind:get`, `@bind:set`, `@bind:event`) or manual `value='@Expr'` plus `@oninput='(ChangeEventArgs e) => ...'`. When inner C# requires double quotes, use single quotes for the HTML attribute rather than escaping quotes inside Razor expressions.

## Current limitations

- The firmware command layer can save tool entries and run touch-off calculations, but the full operator-facing WebUI editing screen is not complete yet.
- Parser-level canned cycle execution is not wired yet; current cycle helpers produce validated lower-level move plans.
- Cycle helper coverage now includes threading, rough turning, finishing, grooving, and peck drilling expansion helpers. Parser-level canned-cycle G-code words are still not wired.
- Encoder hardware pin capture still needs to be wired to the firmware encoder backend core before threading can be used on real hardware.


## ESP32-S3 encoder capture backend

When `lathe.encoder_enable: true`, firmware initializes the configured `encoder_pulse_pin` as an interrupt input and feeds rising-edge pulse timestamps into `Lathe::EncoderSpindleFeedback`. If `encoder_index_pin` is configured, firmware also initializes it as an interrupt input and feeds rising-edge index timestamps into the same feedback object. The active spindle feedback path returns this live encoder feedback while capture is active; otherwise it preserves the default null-feedback behavior.

`ESP421` reports whether encoder capture is active, the configured pulses per revolution, measured RPM, index availability, angular position, revolution count, stale state, and fault state. Hardware validation is still required on the target machine before enabling threading cuts in material.
