# Empirical optimization target for the historical 100 MHz fabric-register core.
# The same-boot address-only hardware control passed with A9/A10/A12 native
# GPIO arrivals of 6.002/5.604/6.169 ns. Apply a 6.5 ns native arrival target
# to all thirteen address bits, without selecting routes or register locations.
# 10 ns period - 3.5 ns output delay = 6.5 ns available arrival budget.
# This excludes uncharacterized pad/package/board delays and is NOT a complete
# SDRAM chip-pin constraint or a timing signoff claim.
create_clock -name FPGA_CLK1_50 -period 20 [get_ports FPGA_CLK1_50]
set_output_delay -clock {ram_clock.clocks[0]} -min 0 [get_ports {SDRAM_A[*]}]
set_output_delay -clock {ram_clock.clocks[0]} -max 3.5 [get_ports {SDRAM_A[*]}]
