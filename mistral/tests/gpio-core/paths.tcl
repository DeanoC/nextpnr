package require ::quartus::project
package require ::quartus::sta
project_open top
create_timing_netlist
read_sdc
set pins [get_pins {*|clk *|clkhi *|clklo}]
foreach corner [get_available_operating_conditions] {
    set_operating_conditions $corner
    update_timing_netlist
    file mkdir native/$corner
    file mkdir native-rf/$corner
    file mkdir native-clock/$corner
    report_timing -setup -from [get_ports {SDRAM_DQ[*]}] -npaths 500 -nworst 100 -detail full_path -show_routing -file native/$corner/input-setup.rpt
    report_timing -setup -to [get_ports {SDRAM_*}] -npaths 500 -nworst 100 -detail full_path -show_routing -file native/$corner/output-setup.rpt
    foreach kind {setup hold} {
        foreach edge {rise fall} {
            report_timing -$kind -${edge}_to [get_registers *] -npaths 2000 -nworst 100 -detail full_path -show_routing -file native-rf/$corner/register-$edge-$kind.rpt
            report_timing -$kind -${edge}_from [get_registers *] -to [get_ports {q[*]}] -npaths 2000 -nworst 100 -detail full_path -show_routing -file native-rf/$corner/fabric-$edge-$kind.rpt
        }
    }
    foreach kind {max min} {
        set opt {}
        if {$kind eq "min"} {set opt -min_path}
        foreach edge {rise fall} {
            report_path {*}$opt -from [get_ports FPGA_CLK1_50] -${edge}_to $pins -npaths 2000 -nworst 100 -show_routing -file native-clock/$corner/$edge-$kind.rpt
        }
    }
}
project_close
