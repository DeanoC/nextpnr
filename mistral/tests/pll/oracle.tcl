# Run with Quartus 17.0.2 quartus_sh -t /absolute/path/oracle.tcl
# in an empty output directory. This is a reference build, not an OSS dependency.
package require ::quartus::project
set fixture [file dirname [file normalize [info script]]]
project_new top -overwrite
set_global_assignment -name FAMILY "Cyclone V"
set_global_assignment -name DEVICE 5CSEBA6U23I7
set_global_assignment -name TOP_LEVEL_ENTITY top
set_global_assignment -name VERILOG_FILE [file join $fixture top.v]
set_global_assignment -name SDC_FILE clocks.sdc
set_global_assignment -name PROJECT_OUTPUT_DIRECTORY output_files
set_global_assignment -name GENERATE_RBF_FILE ON
set_global_assignment -name STRATIXV_CONFIGURATION_SCHEME "PASSIVE SERIAL"
set_global_assignment -name ENABLE_CONFIGURATION_PINS OFF
set_global_assignment -name NUM_PARALLEL_PROCESSORS 4
source [file join $fixture pins.qsf]
# Quartus 17's generic altera_pll wrapper needs this assignment in addition
# to operation_mode, as emitted by its IP generator's QIP file.
set_instance_assignment -name PLL_COMPENSATION_MODE DIRECT -to "*pll*|*"
set sdc [open clocks.sdc w]
puts $sdc {create_clock -name FPGA_CLK1_50 -period 20.000 [get_ports {FPGA_CLK1_50}]}
puts $sdc {derive_pll_clocks}
puts $sdc {derive_clock_uncertainty}
close $sdc
export_assignments
project_close
