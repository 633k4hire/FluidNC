# Lathe Diameter/Radius Validation Audit

FluidNC keeps internal machine and planner X coordinates in radius millimeters. In lathe `G7` diameter mode, user-entered X values are converted to internal radius before normal coordinate, offset, planner, CSS, tool, and cycle handling. In `G8` radius mode, X values are already internal-radius values.

## Coordinate path audit

| Path | Policy | Validation status |
| --- | --- | --- |
| Linear/rapid/arc X words | `G7` X input is converted to internal radius before motion planning. | Covered by conversion helper tests and fixture examples. |
| `G10` coordinate setting | `G7` X offsets are converted before the existing `G10 L2/L20` offset logic stores coordinate data. | Fixture includes `G10 L2` in diameter mode. |
| `G92` temporary offsets | `G7` X is converted before `G92` stores the temporary coordinate offset. | Fixture includes set/reset of `G92` in diameter mode. |
| Probing | Normal `G38.x` probing X targets pass through the same X-word conversion path; probe result remains machine-radius internally. | Fixture includes a commented dry-run probe block for machines with probes. |
| Jogging | Jog X targets use the same parsed X-word conversion path when lathe diameter mode is active. Realtime `$J` senders should label X entry as diameter when `G7` is active and radius when `G8` is active. | UI/operator warning documented; hardware jog validation required. |
| Tool offsets and touch-off | Tool table X geometry/wear is stored internally in radius millimeters; touch-off accepts `MODE=diameter` and converts reference X to radius before storing geometry. | Unit tests cover diameter touch-off and wear preservation. |
| Parser-level cycles | `G70/G71/G75/G76/G83` consume parser-converted X values, so `G7` cycle X words become internal radius before cycle expansion. | Fixture includes diameter-mode cycle examples. |
| CSS | CSS converts internal machine radius back to cutting diameter for RPM calculations. | Unit tests cover radius-to-diameter and CSS diameter math. |
| WebUI | The fallback WebUI touch-off form has an explicit Radius/Diameter selector and warning when firmware reports `G7`. | Fallback UI wraps `ESP421`/`ESP423`; production UI should mirror the same policy. |

## Hardware dry-run checklist

1. Enable lathe mode with encoder disabled, confirm ordinary non-threading `G7/G8` motion dry-runs without alarms.
2. In `G7`, command a safe-air `G0 X20` and confirm internal/controller position corresponds to radius 10 mm while the operator-facing intent is 20 mm diameter.
3. Run `G10 L2` and `G92` fixture blocks in check mode first, then dry-run, and verify no double-conversion of X offsets occurs.
4. Validate touch-off with `ESP423 ... MODE=diameter` and confirm stored geometry X equals half of the entered diameter reference delta after wear is considered.
5. Dry-run `G70/G71/G75/G83` examples in `docs/lathe-fixtures/diameter-radius-coordinate-audit.ngc` and confirm X endpoints match expected internal radius coordinates.
6. Only after the above passes, repeat with encoder-enabled threading and CSS dry-runs.
