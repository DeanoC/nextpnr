# HOST-ONLY CONDITIONAL MODEL TEST; arbitrary budgets, unmeasured load, not physical qualification.
set_output_delay -clock {clocks[0]} -min -1 [get_ports {SDRAM_nWE SDRAM_nRAS SDRAM_nCS SDRAM_nCAS SDRAM_CLK SDRAM_CKE SDRAM_A[11] SDRAM_A[12]}]
set_output_delay -clock {clocks[0]} -max 2 [get_ports {SDRAM_nWE SDRAM_nRAS SDRAM_nCS SDRAM_nCAS SDRAM_CLK SDRAM_CKE SDRAM_A[11] SDRAM_A[12]}]
