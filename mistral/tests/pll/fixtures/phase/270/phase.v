// A single path crosses two related PLL outputs. The runner changes its
// direction and edge polarities to expose each intended setup budget.
module top(input wire FPGA_CLK1_50);
    wire phase0, phase90;
    wire locked;
    wire [31:0] gpo;
    reg launch = 0;
    reg capture = 0;
    // Keep a positive-edge consumer on each PLL output so Yosys emits the
    // primary clock buffers even in the runner's falling-edge variants.
    reg anchor0 = 0, anchor1 = 0;
    always @(posedge phase0) anchor0 <= gpo[1];
    always @(posedge phase90) anchor1 <= gpo[2];
    altera_pll #(
        .reference_clock_frequency("50.0 MHz"), .number_of_clocks(2),
        .output_clock_frequency0("25.0 MHz"), .output_clock_frequency1("25.0 MHz"),
        .phase_shift0("0 ps"), .phase_shift1("30000 ps"),
        .duty_cycle0(50), .duty_cycle1(50), .operation_mode("direct"),
        .fractional_vco_multiplier("false")
    ) pll (.refclk(FPGA_CLK1_50), .rst(1'b0), .outclk({phase90, phase0}), .locked(locked));
    always @(posedge phase0) launch <= gpo[0];
    always @(posedge phase90) capture <= launch;
    cyclonev_hps_interface_mpu_general_purpose hps_gp (
        .gp_in({16'hD719, 12'b0, anchor0, anchor1, locked, capture}), .gp_out(gpo));
endmodule
