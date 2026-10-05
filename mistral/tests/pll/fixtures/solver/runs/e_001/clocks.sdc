create_clock -name ref0 -period 83.333333 [get_ports {refs[0]}]
create_clock -name ref1 -period 76.923077 [get_ports {refs[1]}]
create_clock -name ref2 -period 47.619048 [get_ports {refs[2]}]
create_clock -name ref3 -period 33.333333 [get_ports {refs[3]}]
create_clock -name ref4 -period 30.303030 [get_ports {refs[4]}]
create_clock -name ref5 -period 20.408163 [get_ports {refs[5]}]
derive_pll_clocks
derive_clock_uncertainty
