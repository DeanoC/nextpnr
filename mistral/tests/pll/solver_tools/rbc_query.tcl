# quartus_sh -t rbc_query.tcl IN OUT
# Each IN line: kind|args...  Results are written one line per query to OUT.
#   vco|ref|frac|f0;ph0;d0|f1;ph1;d1...      -> GENERIC_PLL PLL_OUTPUT_CLOCK_FREQUENCY list
#   oc|ref|frac|f0;ph0;d0|f1;ph1;d1...       -> GENERIC_PLL OUTPUT_CLOCK_FREQUENCY (closest values for output 0)
#   fpll|ref|vco|dsm|RULE[,RULE...]          -> CYCLONEV_PLL_CONFIG FRACTIONAL_PLL_* rule values
#   cnt|vco|f|ph|duty|RULE[,RULE...]         -> CYCLONEV_PLL_CONFIG PLL_OUTPUT_COUNTER_* rule values
load_package advanced_pll_legality
set P 5CSEBA6U23I7
set in [open [lindex $argv 0] r]
set out [open [lindex $argv 1] w]
proc outs {fields} {
  set l {}
  foreach o $fields { lappend l {*}[split $o ";"] }
  for {set i [llength $fields]} {$i < 18} {incr i} { lappend l 0 0 50 }
  return $l
}
while {[gets $in line] >= 0} {
  set f [split $line "|"]
  set kind [lindex $f 0]
  if {[catch {
    switch $kind {
      vco {
        set l [list $P [lindex $f 1] [lindex $f 2] fPLL "0.0 MHz" "" "" "" ""]
        lappend l {*}[outs [lrange $f 3 end]]
        set r [get_advanced_pll_legality_legal_values -flow_type MEGAWIZARD -configuration_name GENERIC_PLL -rule_name PLL_OUTPUT_CLOCK_FREQUENCY -param_args $l]
      }
      oc {
        set o0 [split [lindex $f 3] ";"]
        set l [list $P [lindex $f 1] {*}$o0 [lindex $f 2] fPLL "0.0 MHz"]
        set rest [outs [lrange $f 4 end]]
        lappend l {*}[lrange $rest 0 [expr {17*3-1}]]
        set r [get_advanced_pll_legality_legal_values -flow_type MEGAWIZARD -configuration_name GENERIC_PLL -rule_name OUTPUT_CLOCK_FREQUENCY -param_args $l]
      }
      fpll {
        set r {}
        foreach rule [split [lindex $f 4] ","] {
          lappend r [get_advanced_pll_legality_legal_values -flow_type POST_FITTER -configuration_name CYCLONEV_PLL_CONFIG -rule_name FRACTIONAL_PLL_$rule -param_args [list $P [lindex $f 1] [lindex $f 2] [lindex $f 3] Off Auto {0.0 MHz} direct {Global Clock}]]
        }
      }
      cnt {
        set r {}
        foreach rule [split [lindex $f 5] ","] {
          lappend r [get_advanced_pll_legality_legal_values -flow_type POST_FITTER -configuration_name CYCLONEV_PLL_CONFIG -rule_name PLL_OUTPUT_COUNTER_$rule -param_args [list $P fpll_0 [lindex $f 2] [lindex $f 3] [lindex $f 4] [lindex $f 1]]]
        }
      }
      default { set r "BADKIND" }
    }
  } err]} { set r "ERROR $err" }
  puts $out $r
}
close $out
