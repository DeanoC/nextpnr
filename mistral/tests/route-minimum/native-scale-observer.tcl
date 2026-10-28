package require ::quartus::project
package require ::quartus::sta
# Debug output only. Re-annotate the same fitted design without timing overrides.
set_ini_var -name tis_dump_additional_ip_delays on
project_open top
create_timing_netlist
set corners [get_available_operating_conditions]
delete_timing_netlist
foreach corner $corners {
    puts [format {FACTOR_CORNER_BEGIN %s} $corner]
    flush stdout
    create_timing_netlist -force_dat $corner
    read_sdc
    update_timing_netlist
    puts [format {FACTOR_CORNER_END %s} $corner]
    flush stdout
    delete_timing_netlist
}
project_close
