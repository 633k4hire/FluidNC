; Legacy FluidNC ATC tool change macro for the Maijker 5-tool turret.
; Superseded by the maijker_5_station_turret ATC driver.
; Kept only as a reference for the original M62/M63 + fake A-axis workaround.
; Assumptions:
; - Machine starts with tool 1 in position.
; - Tool changes are called with M6 T<target_tool>, where target_tool is 1..5.
; - #<current_tool> is initialized before the first automatic tool change after boot.

; Capture the desired tool from the M6 command.
#<target_tool> = #T

; Safety check: if the target is the same as the current tool, do nothing.
o1 if [#<target_tool> EQ #<current_tool>]
  (Tool already in position - no change required)
  M30
o1 endif

; Calculate the tool difference. If it is negative, add 5 so the turret rotates forward.
#<delta> = [#<target_tool> - #<current_tool>]
o2 if [#<delta> LT 0]
  #<delta> = [#<delta> + 5]
o2 endif

; Each tool increment is represented as 1 mm on the A axis.
#<steps> = [#<delta> * 1]

; Move the turret forward, then reverse slightly for backlash compensation and locking.
M62 P0
G1 A[#<steps> + 0.1] F75
M63 P0
G1 A-0.076 F75

; Update the tracked tool.
#<current_tool> = #<target_tool>

M30
