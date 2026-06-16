# FluidNC Lathe Capability Gap Report

## Executive summary

FluidNC currently looks like a Grbl-derived 3-axis mill/router controller with spindle abstractions and laser-style rate-adjusted output, not a lathe control.  The parser and modal state do not model lathe-specific diameter/radius modes, constant surface speed, spindle-speed clamping, feed-per-revolution, synchronized spindle motion, or threading/canned lathe cycles.  As a result, it can run simple X/Z moves and ordinary `M3/M4/M5 S...` spindle commands, but it does not yet provide the control loops and G-code semantics expected for safe, productive lathe work.

The largest gaps are:

1. **Constant surface speed (`G96/G97`) is absent.** `S` is stored and planned as a fixed RPM, and spindle updates are only triggered by explicit `S` changes or laser-mode/rate-adjusted logic.
2. **No spindle-position feedback or encoder-synchronized motion exists.** Threading, feed-per-rev, rigid tapping, and CSS all need a measured spindle speed/phase source.
3. **Lathe feed modes are missing.** Only inverse time (`G93`) and units-per-minute (`G94`) are implemented; feed-per-revolution (`G95`) is not.
4. **Lathe coordinate semantics are missing.** There is no `G7/G8` diameter/radius mode and no X-diameter display/input model.
5. **Lathe cycles are missing.** Generic canned cycles are explicitly not supported, and lathe-specific cycles such as roughing, finishing, grooving, drilling, and threading are absent.
6. **Tooling is mill-oriented.** Dynamic tool length offset exists, but lathe tool nose radius compensation, tool geometry/wear offsets, orientation, and front/rear turret semantics are not modeled.

## Current implementation observations

### G-code parser and modal model

- Motion modal group 1 contains `G0/G1/G2/G3/G38.x/G80`; there is no threading move (`G32/G33`), canned lathe cycle, or CSS-related modal group in the enum comments.
- Feed mode modal group 5 contains only `G93/G94`; `G95` feed-per-revolution is not parsed.
- Cutter compensation modal group 7 only accepts `G40`; `G41/G42` are documented as unsupported.
- Parser modal state tracks motion, feed rate, units, distance, plane, tool length offset, coordinate system, program flow, coolant, spindle, tool change, I/O, and override, but it has no fields for CSS mode, spindle clamp, diameter/radius mode, feed-per-rev mode, spindle synchronization, threading pass state, or lathe tool nose data.
- `S` is treated as spindle speed and defaults to the previous parser-state RPM when omitted.

### Spindle and planner behavior

- `gc_state.spindle_speed` is updated from `S` and copied to planner data as a fixed spindle speed.
- Runtime spindle changes are synchronized to the planner only when the commanded `S` changes, or through laser/rate-adjusted behavior.
- The stepper preparation layer has a rate-adjusted path that scales spindle output by current feed speed, but the comments and behavior are laser/PWM focused rather than CSS-by-workpiece-diameter focused.
- The base spindle implementation reports `isRateAdjusted() == false`, and there is no generic encoder/closed-loop spindle feedback interface in the base class.
- Multi-spindle/tool-number switching exists, but it selects spindles by tool number and stops the old spindle; it is not a lathe turret/tool-offset system.

## Gap matrix

| Area | Current state | Lathe expectation | Gap / consequence | Priority |
| --- | --- | --- | --- | --- |
| CSS `G96/G97` | No parser modal state or execution for `G96/G97`; `S` is fixed RPM. | `G96 S...` commands surface speed, continuously recomputing RPM from current X diameter; `G97` returns to fixed RPM. | Cannot maintain cutting speed as diameter changes; facing/taper work requires manual RPM edits. | Critical |
| RPM clamp `G50` or equivalent | No spindle max clamp tied to CSS modal state. Existing max spindle settings are device/config level. | CSS must be bounded by programmable max RPM, commonly `G50 S...`. | CSS would be unsafe without a per-program RPM ceiling, especially near centerline. | Critical |
| Spindle feedback | Spindle classes map command speed to device output, but no common encoder speed/phase contract was found. | CSS, threading, feed-per-rev, and diagnostics need measured RPM and sometimes index/phase. | Control cannot synchronize motion to real spindle behavior or verify actual RPM. | Critical |
| Feed per revolution `G95` | Only `G93` and `G94` are implemented. | Lathe turning/thread-adjacent work commonly uses `G95 F...` in units/rev. | Feed changes with spindle RPM are impossible, making lathe feeds less natural and less robust. | Critical |
| Threading moves `G32/G33` | No threading motion mode in parser/planner. | Single-point threading requires spindle-index synchronized Z/X motion. | No reliable threading; user must rely on external/manual methods. | Critical |
| Multi-pass threading cycles | Generic canned cycles and lathe cycles absent. | `G76` or controller-specific threading cycles generate synchronized multi-pass moves. | Even if `G32/G33` were added, programs remain verbose and error-prone. | High |
| Diameter/radius programming `G7/G8` | No modal field or parser handling for lathe diameter mode. | X-axis may be programmed/displayed in diameter while motion occurs in radius. | Lathe CAM/post conventions and operator expectations are not supported. | High |
| Tool nose radius compensation | `G41/G42` explicitly unsupported; only `G40` accepted. | Turning profiles need nose radius compensation and insert orientation. | Manual CAM compensation required; offsets are fragile across tools/inserts. | High |
| Lathe tool offsets | Tool tracking and dynamic TLO exist; no lathe geometry/wear offset model. | Lathe tools need X/Z geometry offsets, wear offsets, nose radius, and orientation. | Tool setting and touch-off workflows are incomplete for turrets/QCTP. | High |
| Lathe canned cycles | `G81-G89` canned cycles explicitly unsupported; lathe-specific rough/finish/groove cycles absent. | Common lathe controls provide roughing, finishing, grooving, peck drilling, and threading cycles. | Basic hand-written paths work; production lathe programming is inefficient. | Medium/High |
| CSS planner integration | Planner block stores one spindle speed; stepper segments map this to device speed. | CSS requires segment-by-segment or interpolation-point RPM updates based on current X radius/diameter and clamps. | Large X moves under CSS would require continuous updates, not just block-level S. | Critical |
| Status/reporting | Status reports feed and spindle speed, but no CSS mode, commanded surface speed, actual RPM, clamp, feed-per-rev, or diameter mode. | Operators need active CSS/fixed RPM mode, commanded/actual RPM, G95 state, and diameter/radius display clarity. | Debugging and safe operation are harder. | Medium |
| Homing/axes conventions | Generic Cartesian axes are supported. | Lathe profiles generally assume X/Z axes, diameter display for X, optional spindle C index, turret axes, limits tailored to lathe geometry. | Machine can move like a lathe but lacks lathe-specific validation and UI semantics. | Medium |

## Recommended implementation roadmap

### Phase 1: Foundation and safety

1. Add lathe feature configuration, for example `machine.type: lathe` or explicit `lathe:` config block.
2. Introduce parser modal state for:
   - spindle speed mode: fixed RPM vs CSS;
   - commanded surface speed and units;
   - CSS max RPM clamp;
   - feed mode including `G95`;
   - X diameter/radius mode.
3. Add a spindle feedback abstraction that can expose:
   - measured RPM;
   - index pulse availability;
   - angular position/phase when supported;
   - stale/fault status.
4. Add safety checks before enabling CSS/threading/feed-per-rev:
   - reject if no encoder/feedback is configured;
   - require CSS RPM clamp;
   - clamp near centerline and define minimum effective diameter;
   - alarm on stale spindle feedback.

### Phase 2: CSS and feed-per-rev

1. Parse and report `G96/G97`.
2. Parse and apply a programmable CSS clamp, likely `G50 S...` if compatible with existing G-code semantics.
3. Compute CSS RPM as:
   - metric: `rpm = (1000 * surface_m_per_min) / (pi * diameter_mm)`;
   - inch: `rpm = (12 * surface_ft_per_min) / (pi * diameter_in)`.
4. Update spindle speed during motion as X diameter changes, ideally at segment generation time so facing and tapers get smooth RPM changes.
5. Implement `G95` by converting feed-per-rev to planner feed using measured or commanded spindle RPM, with clear policy when spindle is stopped or feedback is stale.

### Phase 3: Threading primitives

1. Add synchronized threading motion (`G32`/`G33`) as a new motion mode.
2. Gate threading on encoder index/phase support.
3. Plan motion as a phase-locked relationship between spindle revolutions and axis distance.
4. Add feed-hold/resume constraints for threading; uncontrolled resume in the middle of a thread should be prevented or require re-sync.

### Phase 4: Lathe tool model

1. Add lathe tool table fields for X/Z geometry, X/Z wear, nose radius, and orientation.
2. Define tool-number semantics for lathes without breaking existing multi-spindle behavior.
3. Add touch-off workflows and reporting for lathe offsets.
4. Implement tool nose radius compensation (`G41/G42`) only after the tool table and orientation model are in place.

### Phase 5: Cycles and operator experience

1. Add lathe cycles incrementally, starting with the highest-value primitives:
   - `G76` threading cycle;
   - rough turning/facing cycle;
   - finishing cycle;
   - grooving/peck drilling cycles.
2. Update status reports and WebUI to show lathe-specific modes.
3. Add fixture and unit tests for parser state, CSS RPM math, clamp behavior, diameter/radius transforms, `G95`, and threading safety errors.

## Test and audit recommendations

- Add parser tests that intentionally send currently missing lathe commands and assert either the new modal state or explicit unsupported-command errors.
- Add deterministic unit tests for CSS math at several diameters and unit systems.
- Add planner/stepper tests proving CSS updates across a facing move and clamps at the programmed maximum RPM.
- Add encoder simulation tests for feed-per-rev and threading synchronization before testing on real hardware.
- Add safety tests for centerline CSS, stopped spindle under `G95`, stale encoder feedback, feed hold during threading, and spindle override interactions.

## Bottom line

The shortest path to “lathe capable” is not to start with canned cycles.  First add the spindle feedback, modal state, CSS/clamp math, and feed-per-rev plumbing.  Once those primitives are stable and testable, threading and lathe cycles can be layered on top without building unsafe assumptions into the motion planner.
