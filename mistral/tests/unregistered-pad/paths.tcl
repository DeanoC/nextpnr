package require ::quartus::project
package require ::quartus::sta
project_open top
create_timing_netlist
read_sdc
foreach corner [get_available_operating_conditions] {
 set_operating_conditions $corner
 update_timing_netlist
 file mkdir $corner
 for {set i 0} {$i < 16} {incr i} {
  set port [get_ports [format {dq[%d]} $i]]
  foreach channel {data oe input} {
   foreach edge {rise fall} {
    foreach mode {early late} {
     set args [list -npaths 1 -show_routing -file $corner/$i-$channel-$edge-$mode.rpt]
     if {$mode eq "early"} {lappend args -min_path}
     if {$channel eq "input"} {
      lappend args -${edge}_from $port -to [get_registers [format {capture[%d]} $i]]
     } else {
      lappend args -from [get_registers [format {%s[%d]} $channel $i]] -${edge}_to $port
     }
     report_path {*}$args
    }
   }
  }
 }
 report_clocks -file $corner/clocks.rpt
}
project_close
