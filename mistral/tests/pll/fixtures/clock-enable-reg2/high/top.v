module top(input wire FPGA_CLK1_50);
wire pll_clock, gated_clock, locked, enable_status;
wire [31:0] gpo;
reg sampled_status = 0;
always @(posedge FPGA_CLK1_50) sampled_status <= enable_status;
reg [7:0] count = 0;
always @(posedge gated_clock) count <= count + 1'b1;
altera_pll #(.reference_clock_frequency("50.0 MHz"), .number_of_clocks(1),
.output_clock_frequency0("25.0 MHz"), .phase_shift0("0 ps"), .duty_cycle0(50),
.operation_mode("direct"), .fractional_vco_multiplier("false"))
pll (.refclk(FPGA_CLK1_50), .rst(1'b0), .outclk(pll_clock), .locked(locked));
cyclonev_clkena #(.clock_type("global clock"), .ena_register_mode("double register"),
.ena_register_power_up("high"), .disable_mode("low"), .test_syn("high"))
gate (.inclk(pll_clock), .ena(gpo[0]), .outclk(gated_clock), .enaout(enable_status));
cyclonev_hps_interface_mpu_general_purpose hps_gp (.gp_in({22'b0, sampled_status, locked, count}), .gp_out(gpo));
endmodule
