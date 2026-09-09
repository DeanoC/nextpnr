module top(input wire FPGA_CLK1_50);
    wire [1:0] clocks;
    wire locked;
    wire [31:0] gpo;
    reg [7:0] count0 = 0;
    always @(posedge clocks[0]) count0 <= count0 + 1'b1;
    reg [7:0] count1 = 0;
    always @(posedge clocks[1]) count1 <= count1 + 1'b1;
    altera_pll #(
        .reference_clock_frequency("50.0 MHz"), .number_of_clocks(2),
        .output_clock_frequency0("100.0 MHz"), .phase_shift0("0 ps"), .duty_cycle0(50),
        .output_clock_frequency1("100.0 MHz"), .phase_shift1("7500 ps"), .duty_cycle1(50),
        .operation_mode("direct"), .fractional_vco_multiplier("false")
    ) pll (.refclk(FPGA_CLK1_50), .rst(1'b0), .outclk(clocks), .locked(locked));
    cyclonev_hps_interface_mpu_general_purpose hps_gp (
        .gp_in({15'b0, locked, count1, count0}), .gp_out(gpo));
endmodule
