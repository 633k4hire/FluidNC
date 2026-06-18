# FluidNC Lathe Support Upstream PR Notes

This document summarizes the lathe-support changes carried in the `633k4hire`
FluidNC fork so a future pull request back to upstream FluidNC can be reviewed
without reconstructing the implementation history from chat logs.

## Branch and Commit Context

Current working branch:

- `codex/maijker-lathe-config`

Relevant commits:

- `9a1fcdbf Resolve lathe PR integration` - main lathe-support integration.
- `2eb855b4 Add Maijker mini lathe example config` - Maijker/XZA sample config
  and turret macro added on top of the integration.

The lathe support is feature-gated. Existing mill/router/laser/plasma behavior
is intended to remain unchanged unless a top-level `lathe:` block enables lathe
mode and its optional sub-features.

## User-Facing Capability Added

The integration adds an initial FluidNC lathe capability layer:

- `lathe:` YAML configuration block with explicit feature gates.
- `G95` feed per revolution.
- `G96` constant surface speed and `G97` fixed RPM mode.
- `G7` diameter programming and `G8` radius programming for the configured
  lathe X axis.
- Guarded `G32`/`G33` threading primitive.
- Conservative parser-level lathe helper cycles:
  - `G70` finishing;
  - `G71` rough turning/facing;
  - `G75` grooving;
  - `G76` threading cycle;
  - `G83` peck drilling/boring.
- Lathe tool table data for X/Z geometry, X/Z wear, nose radius, and insert
  orientation.
- Lathe tool touch-off helper and WebUI command wrappers.
- Encoder feedback core for measured RPM, index, angular position, stale state,
  and fault gating.
- Fallback WebUI lathe status/tool/touch-off panel.
- Example XZA/Maijker mini-lathe config and 5-tool turret macro.

## Configuration Changes

Primary files:

- `FluidNC/src/Machine/LatheConfig.h`
- `FluidNC/src/Machine/MachineConfig.cpp`
- `FluidNC/src/Machine/MachineConfig.h`
- `FluidNC/data/config.yaml`
- `example_configs/maijker_xzact_mini_lathe.yaml`

The new `lathe:` block includes:

```yaml
lathe:
  enable: true
  enable_css: true
  enable_feed_per_rev: true
  enable_threading: false
  min_css_diameter_mm: 1.0
  max_css_rpm: 2500
  x_axis: 0
  z_axis: 2
  feedback_stale_ms: 250
  encoder_enable: true
  encoder_pulse_pin: gpio.34
  encoder_index_pin: gpio.35
  encoder_pulses_per_rev: 1024
```

Validation rejects unsafe or inconsistent combinations:

- lathe sub-features without `lathe.enable: true`;
- CSS without a positive `max_css_rpm`;
- CSS without a positive `min_css_diameter_mm`;
- identical X/Z axis assignments;
- encoder capture without lathe mode enabled;
- encoder capture without a pulse pin;
- threading with encoder capture but no index pin.

The current Maijker/XZA machine config intentionally keeps encoder capture and
threading disabled until the physical spindle encoder is installed and verified.

## Core Firmware Changes

Primary files:

- `FluidNC/src/Lathe.h`
- `FluidNC/src/Lathe.cpp`
- `FluidNC/src/LatheEncoder.h`
- `FluidNC/src/LatheEncoder.cpp`
- `FluidNC/src/CMakeLists.txt`
- `FluidNC/src/Main.cpp`
- `FluidNC/src/Alarm.h`

The new `Lathe` module centralizes:

- feature-gate checks and user-facing feature names;
- CSS RPM calculation and clamp helpers;
- feed-per-rev to mm/min conversion;
- diameter/radius X conversion helpers;
- spindle feedback status contracts;
- encoder feedback state calculation;
- tool table load/save/select/touch-off behavior;
- canned-cycle expansion helpers;
- threading synchronization helper math.

The new lathe synchronization alarm is used when threading cannot safely start
or continue because encoder feedback is missing, stale, faulted, or interrupted
by feed hold.

## Parser and Modal State Changes

Primary files:

- `FluidNC/src/GCode.h`
- `FluidNC/src/GCode.cpp`

Parser state now tracks:

- lathe spindle speed mode: fixed RPM or CSS;
- lathe diameter mode: radius or diameter;
- effective commanded RPM after CSS conversion;
- `G95` feed-per-revolution mode.

Important parser behavior:

- `G96/G97` require CSS to be enabled in the `lathe:` config.
- `G95` requires feed-per-rev to be enabled in the `lathe:` config.
- `G7/G8` require lathe mode to be enabled.
- In `G7`, X words are converted from programmed diameter to internal machine
  radius before normal coordinate/offset/planner handling.
- `G32/G33` require threading enabled, `G95`, active spindle, Z-axis motion, and
  healthy measured RPM/index/angular feedback.
- Lathe cycles validate required words before expansion into lower-level motion.
- Threading cycle `G76` is deliberately conservative:
  `G76 X<final-x> Z<final-z> P<passes> F<pitch>`.

## Planner, Stepper, and Spindle Changes

Primary files:

- `FluidNC/src/Planner.h`
- `FluidNC/src/Planner.cpp`
- `FluidNC/src/Stepper.cpp`
- `FluidNC/src/Spindles/Spindle.h`
- `FluidNC/src/Spindles/Spindle.cpp`

Planner blocks now carry lathe CSS and threading metadata.

CSS behavior:

- `G96` stores surface speed while `gc_state.lathe_commanded_rpm` holds the
  effective RPM.
- Motion blocks can carry start/target diameter and max RPM clamp data.
- Stepper segment preparation recomputes CSS RPM as X diameter changes through
  the block.

Feed-per-rev behavior:

- `G95` converts programmed feed to planner feed using healthy measured RPM when
  available, otherwise the effective commanded RPM.

Threading behavior:

- Threading blocks carry pitch, Z start/target, start RPM, sync state, and
  index/revolution data.
- The first segment waits for the next encoder index count before synchronizing.
- Segment progress is driven from spindle revolutions instead of ordinary time
  progression.
- Feed hold during threading raises the lathe sync alarm and ends motion; the
  first implementation does not try to resume mid-thread.

Spindle behavior:

- The base spindle exposes `latheFeedback()` so spindle implementations can
  provide measured RPM/index/angular state.
- The default feedback path is safe: it reports no hardware capability and does
  not allow threading synchronization.

## WebUI and Command API Changes

Primary files:

- `FluidNC/src/WebUI/WebCommands.cpp`
- `embedded/www/tool.html`
- `embedded/www/js/script.js`
- `embedded/tool.html.gz`
- `FluidNC/src/WebUI/NoFile.h`

New WebUI commands:

- `ESP421` - lathe status JSON.
- `ESP422` - save lathe tool data.
- `ESP423` - perform lathe tool touch-off.

`ESP421` reports:

- lathe enabled state;
- `G96/G97` spindle speed mode;
- `G7/G8` diameter mode;
- feed mode;
- programmed `S` and effective RPM;
- CSS clamp and minimum CSS diameter;
- encoder enabled/capture state and pulses per revolution;
- active lathe tool offset/nose data;
- spindle feedback measured RPM, index, angular position, revolution count,
  stale state, and fault state.

The fallback WebUI includes a lathe operator dashboard backed by the same
commands. Any future production UI should consume these firmware commands
instead of parsing free-form status text.

## Example Machine Artifacts

Primary files:

- `example_configs/maijker_xzact_mini_lathe.yaml`
- `example_configs/maijker_tool_change.gcode`

The Maijker/XZA config targets an MKS-DLC32 V2.1 mini-lathe build and includes:

- X/Z axes plus a turret A axis;
- pendant UART configuration;
- spindle PWM configuration;
- 5-tool turret M6 macro hookup;
- lathe config block with CSS/feed-per-rev enabled;
- threading and encoder capture left disabled with `NO_PIN` placeholders.

The turret macro assumes:

- machine starts with tool 1 in position;
- M6 commands use `T1` through `T5`;
- `#<current_tool>` is initialized before the first automatic tool change after
  boot.

## Tests and Fixtures

Primary files:

- `FluidNC/tests/test_unit/LatheScaffoldTest.cpp`
- `FluidNC/tests/test_unit/LatheTestStubs.cpp`
- `FluidNC/tests/Logging.h`
- `docs/lathe-fixtures/*.ngc`

The lathe unit tests cover:

- default feedback/no-hardware behavior;
- CSS metric and inch RPM math;
- CSS clamping;
- feed-per-rev conversion;
- threading feedback capability gating;
- encoder RPM/phase/stale-state calculation;
- configured encoder fallback behavior;
- diameter/radius conversion policy;
- tool table storage/clear/select behavior;
- touch-off geometry calculation;
- cycle expansion and geometry validation;
- spindle-revolution-based threading trajectory helpers.

Fixture programs document expected usage for:

- G76 threading;
- rough turning;
- remaining helper cycles;
- diameter/radius coordinate audits.

## Documentation Added

Primary files:

- `docs/lathe-gap-report.md`
- `docs/lathe-rollout-plan.md`
- `docs/lathe-configuration.md`
- `docs/lathe-diameter-radius-validation.md`
- `docs/lathe-upstream-pr-notes.md`
- `docs/lathe-fixtures/*.ngc`
- `docs/codex-build-environment.md`

The existing lathe docs serve different audiences:

- `lathe-gap-report.md` records the original capability gap.
- `lathe-rollout-plan.md` records the staged implementation plan and phase
  status.
- `lathe-configuration.md` is the user/operator guide.
- `lathe-diameter-radius-validation.md` audits the X diameter/radius policy.
- This file is the future upstream PR summary.

## Build and Validation Status

Known local validation from the XZA workspace:

- `pio run -e wifi_s3` passed after adding the Maijker example config.
- Unit-test source coverage exists for the new lathe helpers, but the upstream
  PR should rerun the FluidNC unit-test suite and all target PlatformIO builds
  in a clean environment before submission.

Hardware validation status:

- No real XZA lathe cutting validation has been completed yet.
- The physical spindle encoder is not installed.
- The Maijker/XZA config keeps `lathe.enable_threading: false`,
  `lathe.encoder_enable: false`, and encoder pins set to `NO_PIN`.

## Known Limitations Before Upstream PR

These items should be reviewed or completed before asking upstream to merge:

- Confirm the preferred G-code dialect for lathe cycles and CSS clamp syntax.
- Decide whether `G50` should be implemented for program-level CSS RPM clamp or
  whether the config clamp is sufficient for the first PR.
- Review parser modal-group choices for `G7/G8`, `G96/G97`, and lathe cycles
  against upstream FluidNC conventions.
- Validate the stepper-time threading implementation on simulated and physical
  encoder feedback before advertising production threading support.
- Validate CSS update rate against PWM and VFD spindle backends.
- Decide whether WebUI generated artifacts should be included in the upstream PR
  or regenerated by the maintainers' preferred build path.
- Confirm NVS persistence format/versioning for the lathe tool table.
- Re-run regression tests proving lathe-disabled configs behave exactly as
  before.
- Add hardware commissioning notes once encoder wiring and low-speed `ESP421`
  feedback are verified.

## Suggested Upstream PR Shape

The current integration is large. For upstream review, consider splitting it:

1. Lathe config gate, modal state, and reporting scaffold.
2. CSS `G96/G97`, `G95`, and diameter/radius conversion helpers.
3. Spindle feedback contract plus encoder feedback core.
4. Tool table and touch-off API.
5. Threading primitive and synchronization alarm behavior.
6. Cycle expansion helpers and fixtures.
7. WebUI/API additions.
8. Example Maijker/XZA config and turret macro.

This split would let upstream review safety-critical motion changes separately
from UI, docs, and machine-specific examples.
