# quartus_sta -t paths.tcl <project> <npaths> [clock-glob]
# Dump the worst setup paths of every operating corner, one row per path
# point, for comparison with nextpnr's timer on the same wiring (#174).
package require ::quartus::project
package require ::quartus::sta
set project [lindex $argv 0]
set npaths [lindex $argv 1]
set clock_glob [expr {[llength $argv] > 2 ? [lindex $argv 2] : "*"}]
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
foreach corner [get_available_operating_conditions] {
    set_operating_conditions $corner
    update_timing_netlist
    set out [open "paths-$corner.tsv" w]
    puts $out [join {path slack from_clock to_clock launch latch arrival required clock_skew data_delay point type rf incr total src src_port src_loc src_type dst dst_port dst_loc dst_type} "\t"]
    set index 0
    foreach_in_collection clock [get_clocks $clock_glob] {
        set paths [get_timing_paths -setup -npaths $npaths -nworst 1 -to_clock $clock -detail full_path]
        foreach_in_collection path $paths {
            set head [list $index [get_path_info -slack $path] [get_clock_info -name [get_path_info -from_clock $path]] \
                          [get_clock_info -name [get_path_info -to_clock $path]] [get_path_info -launch_time $path] \
                          [get_path_info -latch_time $path] [get_path_info -arrival_time $path] \
                          [get_path_info -required_time $path] [get_path_info -clock_skew $path] \
                          [get_path_info -data_delay $path]]
            set point_index 0
            foreach {kind points} [list arrival [get_path_info -arrival_points $path] required [get_path_info -required_points $path]] {
                foreach_in_collection point $points {
                    set row [concat $head [list "$kind:$point_index" [get_point_info -type $point] \
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
    }
    close $out
    puts "DUMPED $corner $index"
}
project_close
