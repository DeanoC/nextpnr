package require ::quartus::project
package require ::quartus::sta
project_open top
create_timing_netlist
read_sdc
foreach corner [get_available_operating_conditions] {
    set_operating_conditions $corner
    update_timing_netlist
    file mkdir $corner
    foreach kind {setup hold} {
        report_timing -$kind -to [get_ports DDR_OUT] -npaths 100 -nworst 20 -detail full_path -file $corner/output-$kind.rpt
        report_timing -$kind -to [get_registers *] -npaths 100 -nworst 20 -detail full_path -file $corner/register-$kind.rpt
    }
}
project_close
