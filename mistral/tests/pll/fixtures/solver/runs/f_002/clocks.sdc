create_clock -name ref0 -period 10.000000 [get_ports {refs[0]}]
create_clock -name ref1 -period 3.225806 [get_ports {refs[1]}]
create_clock -name ref2 -period 3.125000 [get_ports {refs[2]}]
create_clock -name ref3 -period 3.030303 [get_ports {refs[3]}]
create_clock -name ref4 -period 2.941176 [get_ports {refs[4]}]
create_clock -name ref5 -period 2.898551 [get_ports {refs[5]}]
derive_pll_clocks
derive_clock_uncertainty
