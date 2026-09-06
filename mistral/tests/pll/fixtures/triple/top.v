module top(input wire FPGA_CLK1_50);
    wire [2:0] clocks;
    wire locked;
    wire [31:0] gpo;
    reg [7:0] count0 = 0, count1 = 0, count2 = 0;
    always @(posedge clocks[0]) count0 <= count0 + 1'b1;
    always @(posedge clocks[1]) count1 <= count1 + 1'b1;
    always @(posedge clocks[2]) count2 <= count2 + 1'b1;
    altera_pll #(
        .reference_clock_frequency("50.0 MHz"), .number_of_clocks(3),
        .output_clock_frequency0("25.0 MHz"), .output_clock_frequency1("50.0 MHz"),
        .output_clock_frequency2("100.0 MHz"),
        .phase_shift0("0 ps"), .phase_shift1("0 ps"), .phase_shift2("0 ps"),
        .duty_cycle0(50), .duty_cycle1(50), .duty_cycle2(50),
        .operation_mode("direct"), .fractional_vco_multiplier("false")
    ) pll (.refclk(FPGA_CLK1_50), .rst(1'b0), .outclk(clocks), .locked(locked));
    cyclonev_hps_interface_mpu_general_purpose hps_gp (
        .gp_in({7'b0, locked, count2, count1, count0}), .gp_out(gpo));
endmodule
