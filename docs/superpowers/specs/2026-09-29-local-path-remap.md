# Generic local path remapping

Approved task: recover RAM-test OSS timing with generic mapping and placement improvements. No RTL-specific names, locations, masks or Quartus dependency in the compiler.

A legal routed timing report guides an opt-in post-placement pass in a fresh run. This preserves the live packed Context (including pin polarity, clock metadata and carry constraints) instead of importing a saved placement. The pass validates path edges and placements against that Context, composes adjacent ordinary LUTs, preserves their other consumers, and drives a whole LAB group through an equivalent newly placed LUT. Candidate variants may translate the selected FF group by one tile while preserving its z coordinates. All other placements and sequential state stay fixed.

Enumeration is bounded and deterministic. Truth tables incorporate constants, inverted pins and shared input nets; more than six distinct inputs are rejected. Constraints, clocks, I/O destinations, arithmetic/memory LABs and frozen cells are excluded. A small geometric shortlist receives full placement STA and legality/hold checks. An explicit candidate index chooses a trial; a full fresh route and final analogue timing remain authoritative. No timing gain is assumed from estimates. A no-selection/list-only mode must restore original graph, placement, pin state and indexed users.

Qualification: exhaustive Boolean tests, actual backend mutation/rollback tests, default-off neutrality, then current and frozen RTL with fixed compiler/device/seed/options and complete route reports. Adoption requires improved memory timing, required pixel/capture clocks and hold safety. The installed FES lock changes only after qualification.
