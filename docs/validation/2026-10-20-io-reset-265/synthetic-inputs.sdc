# HOST-ONLY TEST VALUES. NOT SDRAM, BOARD, PAD, OR PHYSICAL SIGNOFF BUDGETS.
# Deliberately reference the controller launch clock, not the capture clock.
# Only tests native graph coverage and constraint propagation on the frozen topology.
set_input_delay -clock {clocks[0]} -clock_fall -min 1 [get_ports {SDRAM_DQ[*]}]
set_input_delay -clock {clocks[0]} -clock_fall -max 3 [get_ports {SDRAM_DQ[*]}]
