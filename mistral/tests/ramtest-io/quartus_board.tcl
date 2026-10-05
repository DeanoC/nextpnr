# Run in a retained full OSS-RTL Quartus project; does not refit it.
# quartus_sta -t quartus_board.tcl 100 /tmp/board100 3.3
package require ::quartus::project
package require ::quartus::sta
if {[llength $quartus(args)] != 3} { error {Expected rate, output directory, inverter maximum ns} }
lassign $quartus(args) rate out inverter
if {$rate ni {100 130}} { error {Expected 100 or 130 MHz} }
if {![string is double -strict $inverter] || !($inverter >= 0 && $inverter < 1e6)} {
 error {Expected finite nonnegative inverter delay}
}
project_open top
create_timing_netlist
read_sdc
set memory [get_clocks {*ram_clock*general[0]*PLL_OUTPUT_COUNTER*divclk}]
if {[get_collection_size $memory] != 1} { error {Expected one physical memory clock} }
set source [get_clock_info -targets $memory]
# H=0/L=1: falling fabric clock drives the physical rising pin edge.
create_generated_clock -name sdram_board -source $source -master_clock $memory -divide_by 1 -invert [get_ports SDRAM_CLK]
set tac [expr {$rate == 100 ? 6.0 : 5.4}]
# Explicit diagnostic assumptions: flights 0..0.5ns, margin 0.2ns.
# This clock already includes the fitted mux/pad path; do not add it again.
set_input_delay -clock sdram_board -max [expr {$tac+1.2}] [get_ports {SDRAM_DQ[*]}]
set_input_delay -clock sdram_board -min 2.3 [get_ports {SDRAM_DQ[*]}]
set outputs [remove_from_collection [get_ports {SDRAM_*}] [get_ports SDRAM_CLK]]
set_output_delay -clock sdram_board -max 2.2 [remove_from_collection $outputs [get_ports SDRAM_nCS]]
set_output_delay -clock sdram_board -min -1.5 $outputs
set_output_delay -clock sdram_board -max [expr {2.2+$inverter}] [get_ports SDRAM_nCS]
foreach corner [get_available_operating_conditions] {
 set_operating_conditions $corner
 update_timing_netlist
 file mkdir $out/$corner
 report_clocks -file $out/$corner/clocks.rpt
 foreach kind {setup hold} {
  report_timing -$kind -from [get_registers {*dq_buf*.dq_sample}] -npaths 500 -nworst 100 -detail full_path -file $out/$corner/capture-handoff-$kind.rpt
  report_timing -$kind -from [get_ports {SDRAM_DQ[*]}] -npaths 500 -nworst 100 -detail full_path -file $out/$corner/input-$kind.rpt
  report_timing -$kind -to $outputs -npaths 500 -nworst 100 -detail full_path -file $out/$corner/output-$kind.rpt
 }
}
project_close
