module top(input wire FPGA_CLK1_50);
wire pll_clock, gated_clock, locked;
wire [31:0] gpo;
reg [7:0] count = 0, running_count = 0;
always @(posedge pll_clock) running_count <= running_count + 1'b1;
always @(posedge gated_clock) count <= count + 1'b1;
altera_pll #(.reference_clock_frequency("50.0 MHz"), .number_of_clocks(1),
.output_clock_frequency0("25.0 MHz"), .phase_shift0("0 ps"), .duty_cycle0(50),
.operation_mode("direct"), .fractional_vco_multiplier("false"))
pll (.refclk(FPGA_CLK1_50), .rst(1'b0), .outclk(pll_clock), .locked(locked));
cyclonev_clkena #(.clock_type("global clock"), .ena_register_mode("falling edge"),
.ena_register_power_up("low"), .disable_mode("low"), .test_syn("high"))
gate (.inclk(pll_clock), .ena(gpo[0]), .outclk(gated_clock), .enaout());
cyclonev_hps_interface_mpu_general_purpose hps_gp (.gp_in({15'b0, locked, running_count, count}), .gp_out(gpo));
endmodule
