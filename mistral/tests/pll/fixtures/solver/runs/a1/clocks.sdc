create_clock -name ref0 -period 20.000000 [get_ports {refs[0]}]
create_clock -name ref1 -period 20.000000 [get_ports {refs[1]}]
create_clock -name ref2 -period 20.000000 [get_ports {refs[2]}]
derive_pll_clocks
derive_clock_uncertainty
