package require ::quartus::project
package require ::quartus::sta
project_open top
create_timing_netlist
read_sdc
update_timing_netlist
set out [open "bindings.tsv" w]
puts $out "cell\tinput\tlaunch\tlaunch_location"
foreach_in_collection cell [get_cells -hierarchical target_*] {
    foreach_in_collection pin [get_cell_info -in_pins $cell] {
        set input [get_pin_info -suffix $pin]
        if {$input eq "cin"} {continue}
        set found 0
        foreach_in_collection path [get_timing_paths -setup -from [get_registers launch_*] \
                -to [get_registers capture_*] -through $pin -npaths 1 -nworst 1] {
            set launch [get_path_info -from $path]
            puts $out [join [list [get_cell_info -name $cell] $input \
                [get_node_info -name $launch] [get_node_info -location $launch]] "\t"]
            incr found
        }
        if {$found != 1} {error "Missing direct register witness for [get_pin_info -name $pin]"}
    }
}
close $out
foreach corner [get_available_operating_conditions] {
    set_operating_conditions $corner
    update_timing_netlist
    file mkdir paths/$corner
    foreach analysis {setup hold} {
        foreach edge {rise fall} {
            report_timing -$analysis -${edge}_to [get_registers capture_*] \
                -from [get_registers launch_*] -npaths 64 -nworst 1 \
                -detail full_path -show_routing -file paths/$corner/$analysis-$edge.rpt
        }
    }
}
project_close
