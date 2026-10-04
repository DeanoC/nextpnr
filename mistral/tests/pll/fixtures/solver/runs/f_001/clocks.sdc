create_clock -name ref0 -period 66.666667 [get_ports {refs[0]}]
create_clock -name ref1 -period 40.000000 [get_ports {refs[1]}]
create_clock -name ref2 -period 27.777778 [get_ports {refs[2]}]
create_clock -name ref3 -period 20.000000 [get_ports {refs[3]}]
create_clock -name ref4 -period 16.666667 [get_ports {refs[4]}]
create_clock -name ref5 -period 10.000000 [get_ports {refs[5]}]
derive_pll_clocks
derive_clock_uncertainty
