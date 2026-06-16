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
```

### Required safety rules

- `enable_css`, `enable_feed_per_rev`, and `enable_threading` require `enable: true`.
- CSS requires `max_css_rpm` greater than zero so `G96` cannot accelerate the spindle without a clamp.
- CSS requires `min_css_diameter_mm` greater than zero to avoid infinite RPM near centerline.
- `x_axis` and `z_axis` must be different.
- Threading should remain disabled until spindle feedback reports measured RPM, index pulse, angular position, non-stale state, and no fault.

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

## Tool offsets

Lathe tool data is currently a fixed-size in-memory model. Each entry stores:

- X/Z geometry offsets;
- X/Z wear offsets;
- tool nose radius;
- insert orientation.

The active X/Z lathe offset is applied through the existing TLO vector on `M6` and `M61` while lathe mode is enabled. Tool geometry and wear are summed only when a tool becomes active. X offsets are stored as machine-radius millimeters; diameter-mode touch-off workflows must convert diameter values to radius before storage.

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
- active lathe tool offsets and nose radius;
- spindle feedback measured RPM/index/angular/stale/fault state.

The fallback embedded WebUI tool page includes a Lathe Status panel that calls `ESP421`. The main WebUI should use the same endpoint so it can display lathe state without parsing free-form text.

### Blazor/WebUI implementation note

If a future .NET 8 Blazor WebUI consumes the lathe endpoint, do not put C# lambdas inside quoted Razor attributes such as `@bind-value:get="() => ..."` on native HTML elements. Use .NET 8 binding modifiers (`@bind`, `@bind:get`, `@bind:set`, `@bind:event`) or manual `value='@Expr'` plus `@oninput='(ChangeEventArgs e) => ...'`. When inner C# requires double quotes, use single quotes for the HTML attribute rather than escaping quotes inside Razor expressions.

## Current limitations

- Tool data is not persisted across reboot yet.
- Parser-level canned cycle execution is not wired yet; current cycle helpers produce validated lower-level move plans.
- Encoder hardware backends still need to implement the spindle feedback contract before threading can be used on real hardware.
