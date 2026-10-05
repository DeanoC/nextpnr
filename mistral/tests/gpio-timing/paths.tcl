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
        report_timing -$kind -from [get_ports {dq[*] p1 p2 p3 p5 p7}] -npaths 100 -nworst 20 -detail full_path -file $corner/input-$kind.rpt
        report_timing -$kind -to [get_ports {dq[*] p1 p2 p3 p4 p6 p8}] -npaths 100 -nworst 20 -detail full_path -file $corner/output-$kind.rpt
        report_timing -$kind -to [get_ports {q[*]}] -npaths 100 -nworst 20 -detail full_path -file $corner/fabric-$kind.rpt
        report_timing -$kind -to [get_registers *] -npaths 100 -nworst 20 -detail full_path -file $corner/register-$kind.rpt
        report_timing -$kind -from [get_registers {*DFFLO*}] -npaths 100 -nworst 20 -detail full_path -file $corner/retime-$kind.rpt
    }
}
project_close
