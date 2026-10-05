create_clock -name ref0 -period 1.666667 [get_ports {refs[0]}]
create_clock -name ref1 -period 1.538462 [get_ports {refs[1]}]
create_clock -name ref2 -period 1.428571 [get_ports {refs[2]}]
create_clock -name ref3 -period 1.428571 [get_ports {refs[3]}]
create_clock -name ref4 -period 1.428571 [get_ports {refs[4]}]
derive_pll_clocks
derive_clock_uncertainty
