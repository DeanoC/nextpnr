package require ::quartus::project
package require ::quartus::sta
project_open top
create_timing_netlist
read_sdc
update_timing_netlist
foreach corner [get_available_operating_conditions] {
 set_operating_conditions $corner
 update_timing_netlist
 file mkdir timing-audit/$corner
 foreach kind {recovery removal setup hold} {
  report_timing -$kind -npaths 1000 -nworst 20 -detail full_path -file timing-audit/$corner/$kind.rpt
 }
 report_min_pulse_width -nworst 100 -file timing-audit/$corner/pulse.rpt
}
project_close
