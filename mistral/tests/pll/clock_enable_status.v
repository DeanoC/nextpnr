module top(input wire FPGA_CLK1_50);
    wire pll_clock, gated_clock, locked, gate_status;
    reg sampled_status = 0;
    always @(posedge FPGA_CLK1_50) sampled_status <= gate_status;
    wire [31:0] gp_out;
    reg [7:0] count = 0;
    always @(posedge gated_clock) count <= count + 1'b1;
    altera_pll #(
        .reference_clock_frequency("50.0 MHz"), .number_of_clocks(1),
        .output_clock_frequency0("25.0 MHz"), .phase_shift0("0 ps"),
        .duty_cycle0(50), .operation_mode("direct"), .fractional_vco_multiplier("false")
    ) pll (.refclk(FPGA_CLK1_50), .rst(1'b0), .outclk(pll_clock), .locked(locked));
    cyclonev_clkena #(.clock_type("Global Clock"), .ena_register_mode("falling edge"), .ena_register_power_up("low"))
        gate (.inclk(pll_clock), .ena(gp_out[0]), .outclk(gated_clock), .enaout(gate_status));
    cyclonev_hps_interface_mpu_general_purpose hps_gp (
        .gp_in({22'b0, sampled_status, locked, count}), .gp_out(gp_out));
endmodule
