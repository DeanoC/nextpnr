// Two registers deliberately cross opposite clock edges. The test runner also
// reverses these edges, so each half-cycle budget is independently observable.
module top(input wire FPGA_CLK1_50);
    wire duty_clock, locked;
    wire [31:0] gpo;
    reg launch = 0;
    reg capture = 0;
    altera_pll #(
        .reference_clock_frequency("50.0 MHz"), .number_of_clocks(1),
        .output_clock_frequency0("25.0 MHz"), .phase_shift0("0 ps"),
        .duty_cycle0(25), .operation_mode("direct"),
        .fractional_vco_multiplier("false")
    ) pll (.refclk(FPGA_CLK1_50), .rst(1'b0), .outclk(duty_clock), .locked(locked));
    always @(posedge duty_clock) launch <= gpo[0];
    always @(negedge duty_clock) capture <= launch;
    cyclonev_hps_interface_mpu_general_purpose hps_gp (
        .gp_in({16'hD718, 14'b0, locked, capture}), .gp_out(gpo));
endmodule
