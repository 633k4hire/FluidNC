# FluidNC Lathe Capability Master Rollout Plan

## Purpose

This document is the master roadmap for turning the lathe gap report into an implementation program.  It is intended to be updated at the end of each phase and used as the canonical checklist for scope, dependencies, acceptance criteria, tests, safety gates, and documentation requirements.

The rollout covers five phases:

1. **Foundation and safety**: lathe mode, parser state, spindle feedback contracts, and safety gates.
2. **Constant surface speed and feed-per-revolution**: `G96/G97`, CSS clamping, segment-level speed updates, and `G95`.
3. **Threading primitives**: synchronized `G32/G33` motion and feed-hold/resume rules.
4. **Lathe tool model**: geometry/wear offsets, nose radius/orientation, touch-off workflows, and tool nose compensation readiness.
5. **Cycles and operator experience**: threading/turning cycles, reporting, WebUI, docs, and end-to-end validation.

## Guiding principles

- **Safety before convenience**: do not enable CSS, feed-per-rev, threading, or cycles without the feedback and clamp checks needed to keep the machine within configured limits.
- **Small, testable primitives before canned cycles**: implement parser/planner/spindle primitives first; cycles should expand into already-tested lower-level operations.
- **Feature-gated lathe behavior**: lathe semantics must not break existing router, mill, laser, plasma, or spindle configurations.
- **Deterministic behavior on ESP32-class hardware**: avoid algorithms that depend on unbounded dynamic allocation or heavy work in ISR/timing-critical paths.
- **Operator-visible state**: every new modal behavior must be visible through reports/logs/UI so operators can verify whether the controller is in fixed RPM, CSS, feed-per-min, feed-per-rev, diameter mode, or radius mode.
- **Recoverability**: feed hold, safety door, alarm, reset, and resume behavior must be explicitly defined for each lathe mode.

## Phase status tracker

| Phase | Name | Status | Primary owner | Entry dependency | Exit artifact |
| --- | --- | --- | --- | --- | --- |
| 1 | Foundation and safety | Implemented in scaffold form | TBD | Gap report approved | Lathe feature gate, modal state, spindle feedback interface, safety tests |
| 2 | CSS and feed-per-rev | Implemented in initial firmware path | TBD | Phase 1 complete | `G96/G97`, clamp, `G95`, planner/stepper validation |
| 3 | Threading primitives | Implemented in initial guarded path | TBD | Phase 2 complete plus encoder phase/index validated | `G32/G33` synchronized motion with hold/resume rules |
| 4 | Lathe tool model | Implemented in initial in-memory model | TBD | Phase 1 complete; can overlap Phase 3 after offset architecture is agreed | Tool geometry/wear model and touch-off workflow |
| 5 | Cycles and operator experience | Implemented in initial expansion-helper form | TBD | Phases 2-4 complete enough to provide stable primitives | Lathe cycles, UI/reporting, docs, full validation matrix |

At the end of each phase, update the status tracker, completion notes, known limitations, and follow-up items in this document.

## Cross-phase architecture decisions to make early

### Lathe feature gate

Decide whether lathe behavior is enabled by:

- a top-level machine type such as `machine.type: lathe`;
- a `lathe:` configuration block;
- individual feature flags such as `lathe.css`, `lathe.feed_per_rev`, and `lathe.threading`;
- or a combination of machine type plus feature flags.

Recommended approach: use a top-level lathe capability block with explicit sub-feature flags.  This allows a machine to enable lathe display/tooling semantics without enabling threading or CSS until the required hardware is configured.

### Units and semantics

Define these before implementation:

- CSS metric units: surface meters per minute.
- CSS inch units: surface feet per minute.
- X diameter/radius transform policy.
- Whether internal motion coordinates remain radius-based while reports can display diameter.
- Minimum effective CSS diameter near centerline.
- Interaction between CSS and spindle override.
- Whether `S` under `G96` represents surface speed and `S` under `G97` represents RPM.
- Whether switching from `G96` to `G97` preserves current computed RPM or requires an explicit `S`.

Recommended policy: keep internal machine coordinates in true machine units, add a parser/reporting transform for diameter mode, and store CSS/fixed-RPM mode explicitly in parser state.

### Spindle feedback capability levels

Define a capability enum or equivalent feature flags:

| Capability | Required for | Notes |
| --- | --- | --- |
| Command-only spindle | Basic `M3/M4/M5 S...` | Existing behavior. |
| Measured RPM | CSS diagnostics, feed-per-rev safety | Useful for closed-loop validation. |
| Index pulse | Single-start threading sync | Required for deterministic thread starts. |
| Angular phase/position | Robust threading, multi-start support, future spindle-orientation features | May require hardware-specific backends. |
| Fault/stale detection | CSS, `G95`, threading | Needed for safe alarms. |

### Real-time constraints

Before changing stepper/spindle timing code, document:

- where CSS RPM should be computed;
- how often spindle command updates may be emitted;
- how to avoid excessive Modbus/VFD traffic;
- how PWM/analog spindles differ from VFD spindles;
- what belongs in planner preprocessing versus ISR-adjacent code;
- how stale encoder data is detected without blocking motion.

## Phase 1: Foundation and safety

### Objective

Add the architecture required to represent lathe modes safely without enabling unfinished cutting behavior by default.

### Scope

- Add lathe configuration scaffolding.
- Add parser/modal state fields for lathe concepts.
- Add spindle feedback abstraction and capability reporting.
- Add validation and alarm paths for unsupported lathe operations.
- Add reporting hooks for lathe modal state, even if most modes still report unsupported.
- Add tests proving existing non-lathe behavior remains unchanged.

### Implementation tasks

#### 1. Configuration

- Add a lathe capability block or equivalent feature gate.
- Add config fields for:
  - enable/disable lathe mode;
  - enable/disable CSS;
  - enable/disable feed-per-rev;
  - enable/disable threading;
  - minimum CSS diameter;
  - default/max CSS RPM clamp;
  - X axis lathe role;
  - Z axis lathe role;
  - encoder input configuration;
  - stale feedback timeout.
- Validate configuration at startup.
- Reject impossible combinations, for example threading enabled without an index/phase-capable spindle feedback backend.

#### 2. Parser and modal state

Add explicit parser state for:

- spindle speed mode: fixed RPM vs CSS;
- CSS commanded surface speed;
- CSS max RPM clamp;
- feed mode: inverse time, units/min, units/rev;
- diameter/radius mode;
- lathe mode active/inactive;
- spindle feedback required/not required for the current modal state.

#### 3. Spindle feedback interface

Add a spindle feedback contract that can expose:

- commanded RPM;
- measured RPM;
- feedback timestamp;
- index pulse support;
- angular position support;
- feedback stale/fault state;
- capability flags.

Initial backends can be stubs/no-op for existing spindles.  Existing machines must continue to compile and run without feedback hardware.

#### 4. Safety behavior

Add a common validation function for lathe-only operations.  It should return clear G-code errors or alarms when:

- lathe mode is disabled;
- requested feature is disabled;
- required spindle feedback is missing;
- CSS clamp is missing or invalid;
- minimum CSS diameter is invalid;
- spindle feedback is stale;
- spindle is disabled when a mode requires active spindle motion;
- motion enters a forbidden resume state.

#### 5. Reporting and diagnostics

Add report fields or messages that make the following visible:

- lathe mode enabled/disabled;
- CSS/fixed-RPM state;
- feed-per-min/feed-per-rev state;
- diameter/radius mode;
- CSS clamp;
- feedback capability and stale/fault state.

### Acceptance criteria

- Existing mill/router/laser/plasma configurations behave as before when lathe mode is disabled.
- Lathe-only G-codes return explicit unsupported/disabled errors until implemented.
- Firmware can report whether spindle feedback is present and what capability level it provides.
- Configuration validation prevents enabling unsafe combinations.
- Unit tests cover modal state defaults, config validation, and unsupported command behavior.

### Required tests

- Parser default modal state test.
- Parser unsupported lathe command test with lathe disabled.
- Config validation tests for CSS/threading/feed-per-rev feature combinations.
- Spindle feedback stub tests.
- Existing G-code parser regression tests.
- PlatformIO compile for the target ESP32-S3 profile once code changes begin.

### Phase 1 completion notes

Phase 1 has been implemented as a safety-first scaffold.  The firmware now has a `lathe:` configuration section, parser modal placeholders for lathe spindle-speed and diameter modes, a base spindle feedback stub, and safety validation paths that reject scaffolded lathe-only features instead of executing incomplete behavior.  Active CSS, `G95`, threading, tool nose compensation, and lathe cycles remain intentionally unimplemented for later phases.

Known limitations after Phase 1:

- `G96/G97`, `G95`, and `G7/G8` are recognized as lathe-related scaffold points but still return unsupported-command errors.
- The spindle feedback abstraction currently defaults to a no-hardware `NullSpindleFeedback` implementation.
- No encoder backend, CSS math, feed-per-rev conversion, synchronized threading, or lathe tool-offset behavior is active yet.
- ESP32-S3 PlatformIO validation may be blocked in environments that cannot fetch the configured PlatformIO Espressif platform.

### Phase 1 exit checklist

- [x] Lathe feature gate merged.
- [x] Parser/modal state extended.
- [x] Spindle feedback interface merged.
- [x] Safety validation functions merged.
- [x] Status/reporting hooks added.
- [ ] Unit tests passing.
- [ ] ESP32-S3 build passing.
- [x] Documentation updated.
- [x] This rollout document updated with completion notes.

## Phase 2: Constant surface speed and feed-per-revolution

### Objective

Implement the first functional lathe cutting modes: CSS `G96/G97`, programmable RPM clamp, and `G95` feed-per-revolution.

### Scope

- Parse `G96` and `G97`.
- Parse/apply a program-level spindle RPM clamp.
- Compute CSS RPM from current X diameter.
- Update spindle output as diameter changes during motion.
- Implement `G95` feed-per-revolution conversion.
- Define behavior for spindle overrides, feed holds, parking, and resume.

### CSS requirements

#### Modal behavior

- `G97`: fixed RPM mode. `S` is interpreted as RPM.
- `G96`: CSS mode. `S` is interpreted as surface speed.
- `G96` requires lathe mode enabled.
- `G96` requires a valid clamp before spindle motion is allowed.
- `G96` requires a valid X diameter source.
- `G96` must not compute infinite RPM at or near centerline.

#### RPM math

- Metric: `rpm = (1000 * surface_m_per_min) / (pi * diameter_mm)`.
- Inch: `rpm = (12 * surface_ft_per_min) / (pi * diameter_in)`.
- Clamp final RPM to configured/programmed max.
- Apply spindle min/max speed limits after CSS math.
- Define rounding behavior once and test it.

#### Segment-level updates

For facing/taper moves, CSS must update during the move as X changes.  The planner/stepper integration should:

- compute RPM from the segment's representative X diameter;
- avoid high-frequency updates beyond spindle backend capability;
- respect VFD communication limits;
- avoid unsafe jumps by clamping acceleration/rate of spindle command changes if needed;
- provide deterministic results in tests.

### Feed-per-revolution requirements

- Parse `G95` as units per spindle revolution.
- Continue supporting existing `G93/G94` behavior.
- Convert programmed feed to planner feed using spindle RPM.
- Define whether conversion uses commanded RPM or measured RPM.
- Recommended first implementation: use commanded/effective RPM for deterministic planning, but require measured RPM feedback when safety policy says `G95` needs active spindle verification.
- Alarm or reject `G95` cutting moves if spindle is stopped or required feedback is stale.

### Acceptance criteria

- `G96/G97` parser state works in metric and inch modes.
- CSS RPM math is covered by unit tests for multiple diameters and clamps.
- CSS updates spindle commands across X-changing moves.
- CSS rejects missing clamps and unsafe near-centerline conditions.
- `G95` converts feed-per-rev to planner feed deterministically.
- Existing `G93/G94` tests still pass.

### Required tests

- CSS parser tests.
- CSS metric RPM math tests.
- CSS inch RPM math tests.
- CSS clamp tests.
- CSS near-centerline safety tests.
- CSS segment update tests for facing and taper paths.
- `G95` parser and feed conversion tests.
- Feed hold/resume tests under CSS and `G95`.
- ESP32-S3 PlatformIO build.

### Phase 2 completion notes

Implemented an initial firmware path for Phase 2:

- `G96` and `G97` now update lathe spindle-speed modal state when lathe mode and CSS are enabled.
- CSS RPM math is centralized in the lathe helper API and supports metric and inch surface-speed input, configured near-centerline minimum diameter, and configured maximum RPM clamping.
- CSS planner data is carried into planner blocks, and stepper segment preparation recomputes spindle RPM across X-changing moves using the segment's progress through the block.
- `G95` is represented as a feed-rate mode and converts feed-per-revolution to planner mm/min using measured spindle RPM when valid, otherwise the effective commanded RPM.
- `G93/G94` handling remains separate from `G95`; jogs continue to force `G94`.

Known limitations after Phase 2:

- CSS X-diameter lookup now routes through the configured lathe X-axis helper, but non-standard axis-role configurations still need machine-level validation on real hardware.
- CSS segment updates are implemented in the stepper preparation path, but hardware VFD/encoder backends still need real-machine validation for update-rate limits.
- `G50`-style program-level RPM clamp syntax is not implemented yet; Phase 2 enforces the configured `lathe/max_css_rpm` clamp.
- `G95` can use measured spindle feedback if a backend supplies it, but the null feedback fallback uses commanded/effective RPM for deterministic planning.

### Phase 2 exit checklist

- [x] `G96/G97` implemented and reported.
- [x] CSS clamp implemented and reported.
- [x] CSS RPM math tested.
- [x] Segment-level CSS update path implemented.
- [x] `G95` implemented and reported.
- [x] Safety behavior documented.
- [ ] Unit/integration tests passing.
- [ ] ESP32-S3 build passing.
- [x] Operator documentation updated in this rollout plan.
- [x] This rollout document updated with completion notes.

## Phase 3: Threading primitives

### Objective

Add synchronized threading moves as safe, testable primitives before adding multi-pass canned threading cycles.

### Scope

- Implement `G32`/`G33` or the selected FluidNC-compatible threading primitive.
- Lock axis motion to spindle index/phase.
- Define synchronization acquisition, loss-of-sync alarms, feed hold behavior, and resume behavior.
- Add encoder simulation tests.

### Requirements

#### Parser behavior

- Add a threading motion mode.
- Require lathe mode enabled.
- Require spindle feedback with index or phase capability.
- Require active spindle motion unless explicitly supporting spindle-start synchronization.
- Require a valid pitch/feed value.
- Reject conflicting modal combinations.

#### Planner behavior

- Convert pitch into synchronized axis distance per spindle revolution.
- Start each threading pass at a deterministic spindle phase.
- Detect stale/lost feedback.
- Avoid blending threading motion with non-threading moves.
- Define acceleration/deceleration entry and exit behavior.

#### Feed hold and resume

Threading cannot be treated like ordinary linear interpolation.  Define and implement one of these policies:

1. **Conservative first release**: feed hold during threading causes controlled stop and requires program restart/re-sync; automatic resume is rejected.
2. **Advanced release**: feed hold parks safely, waits for index, and resumes on the same phase.

Recommended first release: conservative policy.  It is easier to reason about and safer.

### Acceptance criteria

- Threading command is rejected without feedback/index support.
- A simulated encoder can drive a deterministic thread path.
- Lost feedback during threading produces an alarm.
- Feed hold behavior is deterministic and documented.
- Non-threading motion remains unaffected.

### Required tests

- Parser tests for valid/invalid threading blocks.
- Encoder simulation tests.
- Single-pass threading synchronization tests.
- Lost feedback alarm tests.
- Feed hold/resume policy tests.
- Planner blending isolation tests.
- ESP32-S3 PlatformIO build.

### Phase 3 completion notes

Implemented an initial guarded threading primitive path:

- `G32` and `G33` parse into a lathe threading motion primitive when `lathe/enable_threading` is enabled.
- Threading blocks require `G95`, an active spindle, axis motion, measured RPM, an index pulse, angular-position feedback, and non-stale/non-fault feedback status before motion is accepted.
- Threading metadata is carried into planner blocks with pitch and starting RPM, and the spindle feedback interface must explicitly acknowledge threading-start synchronization before motion is accepted.
- The stepper preparation path checks threading feedback during execution and raises a lathe synchronization alarm if feedback is lost, stale, or faulted.
- The first-release feed-hold policy is conservative: threading moves are marked as no-feed-override motion, and any hold/stop requires restart/re-sync rather than automatic phase resume.

Known limitations after Phase 3:

- No concrete encoder hardware backend is implemented in this phase; the spindle-feedback contract now includes a synchronization-start hook for one, and the null feedback backend intentionally rejects threading.
- Threading is represented as a guarded synchronized primitive over the existing linear motion path; deeper phase-locked step scheduling should be validated with the first encoder backend before declaring hardware threading production-ready.
- `G76` and other canned threading cycles remain explicitly out of scope for this phase.

### Phase 3 exit checklist

- [x] Threading parser mode implemented.
- [x] Encoder/index gating implemented.
- [x] Synchronized motion primitive implemented in guarded planner metadata path.
- [x] Lost-sync alarm implemented.
- [x] Feed hold/resume policy implemented and documented.
- [x] Simulation/helper tests added for feedback gating.
- [ ] ESP32-S3 build passing.
- [x] Operator documentation updated in this rollout plan.
- [x] This rollout document updated with completion notes.

## Phase 4: Lathe tool model

### Objective

Add a lathe-appropriate tool data model and workflows so operators can set, report, and compensate turning tools correctly.

### Scope

- Add lathe tool geometry offsets.
- Add wear offsets.
- Add nose radius and insert orientation fields.
- Define lathe tool number semantics.
- Add touch-off workflows.
- Prepare for tool nose radius compensation.

### Tool table data requirements

Each lathe tool should be able to store:

- tool number;
- X geometry offset;
- Z geometry offset;
- X wear offset;
- Z wear offset;
- nose radius;
- insert orientation/quadrant;
- front/rear turret or toolpost orientation if needed;
- optional spindle association if the existing multi-spindle model remains relevant;
- optional comments/labels for UI display.

### Offset application policy

Define:

- whether geometry and wear offsets are summed before planner submission;
- how offsets interact with work coordinate systems;
- how offsets interact with diameter/radius mode;
- how offsets are persisted;
- how reset, homing, alarm, and tool changes affect active offsets.

### Tool nose compensation readiness

Do not implement `G41/G42` until the following are stable:

- active tool nose radius;
- insert orientation;
- path direction conventions;
- compensation entry/exit rules;
- alarm behavior for impossible compensation geometry.

### Acceptance criteria

- Lathe tool offsets can be stored, selected, reported, and applied.
- Wear offsets can be adjusted without changing geometry offsets.
- Tool changes update active lathe offsets deterministically.
- Diameter/radius mode interactions are tested.
- Existing mill tool behavior remains compatible.

### Required tests

- Tool table persistence/config tests.
- Tool selection tests.
- Geometry offset application tests.
- Wear offset application tests.
- Diameter/radius mode interaction tests.
- Reset/alarm/tool-change state tests.
- ESP32-S3 PlatformIO build.

### Phase 4 completion notes

Implemented an initial lathe tool model:

- Added a fixed-size in-memory lathe tool data model with X/Z geometry offsets, X/Z wear offsets, nose radius, and insert orientation.
- Tool geometry and wear are summed into an active X/Z offset when a lathe tool is selected.
- Active lathe X/Z offsets are applied through the existing tool length offset vector on lathe `M6`/`M61` selection, so normal work coordinate and G92 math continues to use the established `WPos = MPos - WCS - G92 - TLO` path.
- The active lathe tool offset is reported in the g-code mode report while lathe mode is enabled.
- Diameter/radius interaction is defined by storing machine offsets in radius millimeters and providing a conversion helper for diameter-programmed X touch-off values.
- Existing mill/router behavior remains unchanged when `lathe/enable` is false; existing spindle-by-tool-number selection still runs before lathe offsets are applied, so lathe tool numbering currently coexists with the existing tool/spindle mechanism by sharing the selected `T` number.

Touch-off workflow documentation:

- First-release touch-off should calculate geometry offsets from probed/touched X/Z positions and store them with `Lathe::set_tool_data()`.
- X touch-off values from diameter-mode operator workflows must be converted through the diameter-to-radius helper before storage.
- Wear offsets should be adjusted separately from geometry offsets and are summed only when a tool becomes active.
- Tool nose radius/orientation are stored and reported, but `G41/G42` tool nose radius compensation remains out of scope until Phase 5+ readiness criteria are satisfied.

Known limitations after Phase 4:

- Tool data is fixed-size and now persisted in NVS through the firmware API; firmware touch-off math is implemented, while operator-facing UI editing remains future work.
- There is no G-code syntax yet for writing lathe tool table entries directly from a program.
- Front/rear turret semantics are represented only by insert orientation at this stage.
- Basic `G7`/`G8` diameter/radius programming is implemented for X words by converting programmed diameter values into internal machine-radius coordinates before normal offset/planner handling; hardware/CAM validation is still required.

### Phase 4 exit checklist

- [x] Lathe tool data model implemented.
- [x] Geometry offsets implemented.
- [x] Wear offsets implemented.
- [x] Nose radius/orientation stored and reported.
- [x] Touch-off workflow implemented or documented.
- [x] Compatibility tests/checks passing where local static checks are available.
- [ ] ESP32-S3 build passing.
- [x] Operator documentation updated in this rollout plan.
- [x] This rollout document updated with completion notes.

## Phase 5: Cycles and operator experience

### Objective

Build higher-level lathe productivity features on top of the primitives delivered in Phases 1-4.

### Scope

- Implement priority lathe cycles.
- Expand status reporting.
- Update WebUI/operator documentation.
- Add end-to-end fixture tests.
- Validate realistic lathe jobs in simulation and on controlled hardware.

### Cycle implementation order

Recommended order:

1. `G76` threading cycle, built on Phase 3 threading primitive.
2. Rough turning/facing cycle.
3. Finishing cycle.
4. Grooving cycle.
5. Peck drilling / boring helper cycles.
6. Optional future cycles after operator feedback.

### Cycle design rules

- Cycles should expand into lower-level validated primitives.
- Cycle-generated moves must be visible in debug/test mode.
- Cycles must validate geometry before motion.
- Cycles must respect CSS, feed-per-rev, tool offsets, and diameter/radius mode.
- Cycles must produce clear alarms for impossible or unsafe inputs.

### Reporting and UI

Expose:

- active lathe mode;
- fixed RPM vs CSS;
- commanded surface speed;
- computed commanded RPM;
- measured RPM if available;
- CSS clamp;
- feed mode and effective feed;
- diameter/radius mode;
- active tool geometry/wear/nose radius summary;
- threading sync status;
- spindle feedback stale/fault status.

### Documentation

Add user-facing documentation for:

- lathe configuration;
- encoder/spindle feedback wiring;
- CSS setup and safety clamp;
- `G95` usage;
- threading setup and limitations;
- tool setting and offsets;
- canned cycle examples;
- troubleshooting alarms.

### Acceptance criteria

- At least one threading cycle and one roughing/finishing workflow are implemented and tested.
- UI/status reports show enough state for safe operation.
- Documentation includes realistic examples.
- Fixture tests cover complete lathe programs.
- Regression tests show existing machine types remain unaffected.

### Required tests

- Cycle parser tests.
- Cycle geometry validation tests.
- Cycle expansion tests.
- Fixture tests for full lathe programs.
- UI/report formatting tests where applicable.
- Regression tests for router/mill/laser/plasma behavior.
- ESP32-S3 PlatformIO build.

### Phase 5 completion notes

Implemented the first safe cycle layer as fixed-size expansion helpers rather than parser-direct motion execution:

- Added a G76-style threading-cycle expansion helper that validates pitch, pass count, X/Z geometry, and expands into lower-level threading and rapid-return moves.
- Added a rough-turning/finishing expansion helper that validates geometry/feed/depth and expands into lower-level linear roughing, return, and optional finish moves.
- Cycle plans use fixed-size move buffers to avoid unbounded allocation on ESP32-class targets.
- Added fixture programs under `docs/lathe-fixtures/` showing realistic expanded threading and rough-turning workflows.
- Existing status reports from Phases 2-4 expose lathe mode, CSS/fixed RPM state, G95, active tool offsets, and threading alarm state; Phase 5 documentation adds the `ESP421` WebUI/API lathe status endpoint and fallback WebUI panel.

Known limitations after Phase 5:

- Cycle helpers are not yet wired to parser-level canned-cycle G-code execution. This is intentional until encoder-backed threading and persistent tool tables are validated on hardware.
- `G76` syntax compatibility, roughing-cycle dialect choices, grooving, peck drilling, and WebUI editing remain future work.
- Fixture programs document expanded lower-level motion rather than executing a new canned-cycle parser command.
- Complete diameter/radius behavior now covers basic X-word programming, CSS diameter conversion, and firmware touch-off math, but operator touch-off screens and WebUI tool-table editing still need to wrap the firmware helper before first-class release.

### Phase 5 exit checklist

- [x] Priority cycles implemented as validated expansion helpers.
- [x] Cycle safety validation implemented.
- [x] Status reports updated by prior lathe modal/tool/alarm reporting work and documented here.
- [x] WebUI/operator experience updated with `ESP421`, fallback UI panel, and configuration guide.
- [x] Fixture examples added for complete lathe programs.
- [ ] ESP32-S3 build passing.
- [x] User documentation updated in this rollout plan, fixture examples, and `docs/lathe-configuration.md`.
- [x] This rollout document updated with final completion notes.

## Program-level validation matrix

| Feature | Unit tests | Integration tests | Fixture tests | Hardware validation | Required before release |
| --- | --- | --- | --- | --- | --- |
| Lathe config gate | Yes | Yes | No | Basic boot check | Phase 1 |
| Spindle feedback interface | Yes | Yes | No | Encoder/backend check | Phase 1 |
| CSS math | Yes | Yes | Yes | Controlled spindle check | Phase 2 |
| CSS segment updates | Yes | Yes | Yes | Facing/taper dry run | Phase 2 |
| `G95` | Yes | Yes | Yes | Feed-per-rev dry run | Phase 2 |
| Threading primitive | Yes | Yes | Yes | Low-speed air-cut thread | Phase 3 |
| Tool offsets | Yes | Yes | Yes | Touch-off dry run | Phase 4 |
| Tool nose data | Yes | Yes | No | Operator workflow check | Phase 4 |
| Cycles | Yes | Yes | Yes | Supervised air-cut then material test | Phase 5 |
| Reporting/UI | Yes | Yes | Optional | Operator review | Phase 5 |

## Release gates

A lathe-capable release should not be marked production-ready until:

- configuration validation prevents unsafe combinations;
- CSS requires and enforces an RPM clamp;
- encoder feedback stale/fault states are tested;
- threading lost-sync behavior alarms reliably;
- feed hold/resume behavior is documented and tested for every lathe mode;
- status reports expose active lathe modes;
- existing non-lathe configurations pass regression testing;
- ESP32-S3 firmware builds pass;
- at least one controlled hardware validation pass has been documented.

## Documentation update process after each phase

At the end of every phase:

1. Update the phase status tracker.
2. Add completion notes under the phase section.
3. Record deviations from the original plan.
4. Record known limitations and follow-up issues.
5. Link test results or CI runs.
6. Update user-facing docs if behavior changed.
7. Update examples and fixture programs.
8. Re-check the release gates.

## Open questions

- What exact G-code dialect should FluidNC target for lathe features: LinuxCNC-like, Fanuc-like, Grbl-compatible extensions, or a documented FluidNC subset?
- Should `G50` be used for CSS spindle clamp, or should FluidNC expose a different syntax to avoid conflicts?
- Should `G7/G8` be diameter/radius mode, or should FluidNC use a configuration-only display transform?
- Which ESP32 encoder peripheral/input path is preferred for spindle feedback on ESP32-S3?
- What is the minimum acceptable spindle feedback capability for CSS: commanded RPM only, measured RPM, or measured RPM plus stale detection?
- Should VFD spindles receive CSS updates at every planner segment, at a rate-limited interval, or only at block boundaries?
- What is the first supported threading policy: conservative no-resume threading or phase-resynchronized resume?
- How should lathe tool numbering coexist with the current spindle-by-tool-number switching behavior?

## Initial milestone proposal

| Milestone | Target deliverable | Suggested validation |
| --- | --- | --- |
| M1 | Lathe config and modal scaffolding | Parser/config unit tests; no behavior change with lathe disabled |
| M2 | Spindle feedback interface and stub backend | Unit tests plus startup/reporting checks |
| M3 | CSS parser/math/clamp | CSS math and parser tests |
| M4 | CSS planner/stepper updates | Facing/taper fixture tests and hardware dry run |
| M5 | `G95` feed-per-rev | Feed conversion tests and dry run |
| M6 | Threading primitive | Encoder simulation and air-cut test |
| M7 | Lathe tool table and offsets | Tool offset tests and touch-off dry run |
| M8 | First lathe cycle | Cycle expansion and full fixture program |
| M9 | UI/reporting/docs completion | Operator review and release-gate checklist |

## Relationship to the gap report

The gap report identifies what is missing.  This rollout plan defines how to close those gaps through staged implementation, safety validation, and release gates.  The gap report should remain the high-level audit, while this document should be updated continuously as the work proceeds.
