package require ::quartus::project
package require ::quartus::sta
project_open top
create_timing_netlist
read_sdc
foreach corner [get_available_operating_conditions] {
 set_operating_conditions $corner
 update_timing_netlist
 set out [open "arcs-$corner.tsv" w]
 puts $out "launch\tedge\tcell\tlocation\twysiwyg\tsrc\tdst\tinput\toutput\trr_max\trf_max\tfr_max\tff_max"
 array unset seen
 set paths [get_timing_paths -setup -npaths 3000 -nworst 100 -detail full_path]
 foreach_in_collection path $paths {
  foreach_in_collection point [get_path_info -arrival_points $path] {
   if {[get_point_info -type $point] ne "cell"} {continue}
   set edge [get_point_info -edge $point]
   if {$edge eq "" || [info exists seen($edge)]} {continue}
   set seen($edge) 1
   set src [get_edge_info -src $edge]
   set dst [get_edge_info -dst $edge]
   if {[catch {set cell [get_pin_info -parent_cell $src]}]} {continue}
   set typ [get_cell_info -wysiwyg_type $cell]
   if {![string match *lcell* $typ]} {continue}
   set row [list [get_node_info -name [get_path_info -from $path]] $edge [get_cell_info -name $cell] [get_cell_info -location $cell] $typ [get_pin_info -name $src] [get_pin_info -name $dst] [get_pin_info -suffix $src] [get_pin_info -suffix $dst]]
   foreach rf {rr rf fr ff} {lappend row [get_edge_info -delay -max -$rf $edge]}
   puts $out [join $row "\t"]
  }
 }
 close $out
 puts "EXTRACTED $corner [array size seen]"
}
project_close
