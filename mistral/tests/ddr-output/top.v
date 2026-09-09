module top #(parameter WITH_PLL=0, INVERTED=0, MINIMAL=0)(input FPGA_CLK1_50, output DDR_OUT);
 wire clock_out;
 generate if(WITH_PLL) begin
  altera_pll #(.reference_clock_frequency("50.0 MHz"),.number_of_clocks(1),
   .output_clock_frequency0("74.25 MHz"),.fractional_vco_multiplier("true"),
   .operation_mode("direct"),.phase_shift0("0 ps"),.duty_cycle0(50))
  pll(.refclk(FPGA_CLK1_50),.rst(1'b0),.outclk(clock_out),.locked());
 end else assign clock_out=FPGA_CLK1_50;
 if(!MINIMAL) begin
  reg [7:0] beat_ref=0, beat_out=0;
  always @(posedge FPGA_CLK1_50) beat_ref<=beat_ref+1'b1;
  always @(posedge clock_out) beat_out<=beat_out+1'b1;
  cyclonev_hps_interface_mpu_general_purpose gp(.gp_in({16'hDD01,beat_ref,beat_out}),.gp_out());
 end endgenerate
 altddio_out #(.width(1),.intended_device_family("Cyclone V"),
  .power_up_high("OFF"),.oe_reg("UNREGISTERED"),.extend_oe_disable("OFF"),.invert_output("OFF"))
 ddr(.datain_h(!INVERTED),.datain_l(INVERTED!=0),.outclock(clock_out),.outclocken(1'b1),
  .aset(1'b0),.aclr(1'b0),.sset(1'b0),.sclr(1'b0),.oe(1'b1),.dataout(DDR_OUT),.oe_out());
endmodule
