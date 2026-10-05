package require ::quartus::project
package require ::quartus::sta
project_open top
create_timing_netlist
read_sdc
foreach corner [get_available_operating_conditions] {
    set_operating_conditions $corner
    update_timing_netlist
    set folder clock-requirements/$corner
    file mkdir $folder
    set stream [open $folder/requirements.tsv w]
    puts $stream "node\tlocation\tperiod_ns\thigh_ns\tlow_ns\tsynchronous_sources\tdata_destinations"
    foreach_in_collection reg [get_registers *] {
        set sources {}
        foreach edge [get_register_info -synch_edges $reg] {
            lappend sources [get_node_info -name [get_edge_info -src $edge]]
        }
        set destinations {}
        foreach edge [get_register_info -fanout_edges $reg] {
            lappend destinations [get_node_info -name [get_edge_info -dst $edge]]
        }
        puts $stream [join [list [get_register_info -name $reg] [get_node_info -location $reg] \
            [get_register_info -tmin $reg] [get_register_info -tch $reg] [get_register_info -tcl $reg] \
            [join $sources ,] [join $destinations ,]] "\t"]
    }
    close $stream
    report_min_pulse_width -nworst 1000 -detail full_path -file $folder/pulse.rpt [get_registers *]
}
project_close
