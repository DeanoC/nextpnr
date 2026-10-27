package require ::quartus::project
package require ::quartus::sta
project_open top
create_timing_netlist
read_sdc
file mkdir arcs
foreach corner [get_available_operating_conditions] {
    set_operating_conditions $corner
    update_timing_netlist
    set out [open "arcs/$corner.tsv" w]
    puts $out "cell\tlocation\twysiwyg\tsrc\tdst\tinput\toutput\tunate\trr_min\trr_max\trf_min\trf_max\tfr_min\tfr_max\tff_min\tff_max"
    set count 0
    array unset seen
    foreach_in_collection cell [get_cells -hierarchical target_*] {
        set name [get_cell_info -name $cell]
        foreach_in_collection pin [get_cell_info -in_pins $cell] {
            foreach edge [get_node_info -fanout_edges $pin] {
                if {[info exists seen($edge)]} {continue}
                set src [get_edge_info -src $edge]
                set dst [get_edge_info -dst $edge]
                if {[get_node_info -type $dst] ne "pin" || [get_pin_info -parent_cell $dst] ne $cell} {continue}
                if {[get_pin_info -parent_cell $src] ne $cell} {error "Primitive input identity mismatch: $name $edge"}
                set seen($edge) 1
                set row [list $name [get_cell_info -location $cell] [get_cell_info -wysiwyg_type $cell] \
                    [get_pin_info -name $src] [get_pin_info -name $dst] \
                    [get_pin_info -suffix $src] [get_pin_info -suffix $dst] [get_edge_info -unateness $edge]]
                foreach transition {rr rf fr ff} {
                    foreach bound {min max} {
                        lappend row [get_edge_info -delay -$bound -$transition $edge]
                    }
                }
                puts $out [join $row "\t"]
                incr count
            }
        }
    }
    close $out
    if {!$count} {error "No primitive edges at $corner"}
    puts "EXTRACTED $corner $count"
}
project_close
