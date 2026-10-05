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
        report_timing -$kind -from [get_ports {SDRAM_DQ[*]}] -npaths 500 -nworst 100 -detail full_path -file $corner/input-$kind.rpt
        report_timing -$kind -to [get_ports {SDRAM_*}] -npaths 500 -nworst 100 -detail full_path -file $corner/output-$kind.rpt
        report_timing -$kind -to [get_registers *] -npaths 500 -nworst 100 -detail full_path -file $corner/register-$kind.rpt
        report_timing -$kind -to [get_ports {q[*]}] -npaths 500 -nworst 100 -detail full_path -file $corner/fabric-$kind.rpt
    }
}
project_close
