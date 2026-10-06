package require ::quartus::project
package require ::quartus::sta
# Run in an existing fitted project, with an absolute output directory.
project_open top
create_timing_netlist
read_sdc
set out [lindex $quartus(args) 0]
set regs [get_registers {*hps_ddr*f2sdram*}]
if {[get_collection_size $regs] == 0} { error {No fitted HPS SDRAM registers} }
foreach corner [get_available_operating_conditions] {
    set_operating_conditions $corner
    update_timing_netlist
    file mkdir $out/$corner
    foreach kind {setup hold} {
        report_timing -$kind -from $regs -npaths 2000 -nworst 20 -detail full_path -file $out/$corner/output-$kind.rpt
        report_timing -$kind -to $regs -npaths 2000 -nworst 20 -detail full_path -file $out/$corner/input-$kind.rpt
    }
}
project_close
