# quartus_sta -t dump.tcl <project>
# Dump every M10K read path (RAM -> so_* capture register) and every M10K write
# path (si_* source register -> RAM), setup and hold, for every operating corner,
# one row per path point (same columns as mistral/tests/quartus-sta/paths.tcl).
package require ::quartus::project
package require ::quartus::sta
set project [lindex $argv 0]
project_open $project
create_timing_netlist
read_sdc
update_timing_netlist
proc pin_row {pin} {
    if {[catch {set cell [get_pin_info -parent_cell $pin]}]} {
        if {[catch {set name [get_node_info -name $pin]}]} {set name ""}
        return [list $name "" "" ""]
    }
    return [list [get_pin_info -name $pin] [get_pin_info -suffix $pin] [get_cell_info -location $cell] \
                [get_cell_info -wysiwyg_type $cell]]
}
proc dump {out kind index paths} {
    foreach_in_collection path $paths {
        set head [list $kind $index [get_path_info -slack $path] [get_clock_info -name [get_path_info -from_clock $path]] \
                      [get_clock_info -name [get_path_info -to_clock $path]] \
                      [get_node_info -name [get_path_info -from $path]] [get_node_info -name [get_path_info -to $path]]]
        set point_index 0
        foreach {side points} [list arrival [get_path_info -arrival_points $path] required [get_path_info -required_points $path]] {
            foreach_in_collection point $points {
                set row [concat $head [list "$side:$point_index" [get_point_info -type $point] \
                                           [get_point_info -rise_fall $point] [get_point_info -incr $point] \
                                           [get_point_info -total $point]]]
                set edge [get_point_info -edge $point]
                if {$edge eq ""} {
                    if {[catch {set name [get_node_info -name [get_point_info -node $point]]}]} {set name ""}
                    set row [concat $row [list $name "" "" "" "" "" "" ""]]
                } else {
                    set row [concat $row [pin_row [get_edge_info -src $edge]] [pin_row [get_edge_info -dst $edge]]]
                }
                puts $out [join $row "\t"]
                incr point_index
            }
        }
        incr index
    }
    return $index
}
foreach corner [get_available_operating_conditions] {
    set_operating_conditions $corner
    update_timing_netlist
    set out [open "char-$corner.tsv" w]
    puts $out [join {analysis path slack from_clock to_clock from to point type rf incr total src src_port src_loc src_type dst dst_port dst_loc dst_type} "\t"]
    set n 0
    foreach analysis {setup hold} {
        set rd [get_timing_paths -$analysis -to [get_registers {so_*}] -npaths 100000 -nworst 1 -detail full_path]
        set n [dump $out "$analysis-read" $n $rd]
        set wr [get_timing_paths -$analysis -from [get_registers {si_*}] -npaths 100000 -nworst 1 -detail full_path]
        set n [dump $out "$analysis-write" $n $wr]
    }
    close $out
    puts "DUMPED $corner $n"
}
project_close
