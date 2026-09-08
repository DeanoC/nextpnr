// Mixed-width SDP diagnostic. GPO: addr[9:0], seed[25:16], window[27:26],
// read clock gate[29], read enable[30], write enable[31].
module top #(parameter UNIT=10, WLANES=4, RLANES=1)(input FPGA_CLK1_50);
 localparam WA=10-$clog2(WLANES), RA=10-$clog2(RLANES);
 wire [31:0] gp_in, gp_out;
 wire pll_clock, read_clock, locked;
 altera_pll #(.reference_clock_frequency("50.0 MHz"), .number_of_clocks(1),
 .output_clock_frequency0("25.0 MHz"), .phase_shift0("0 ps"), .duty_cycle0(50),
 .operation_mode("direct"), .fractional_vco_multiplier("false"))
 pll(.refclk(FPGA_CLK1_50), .rst(1'b0), .outclk(pll_clock), .locked(locked));
 cyclonev_clkena #(.clock_type("global clock"), .ena_register_mode("falling edge"),
 .ena_register_power_up("high"), .disable_mode("low"), .test_syn("high"))
 gate(.inclk(pll_clock), .ena(gp_out[29]), .outclk(read_clock), .enaout());
 (* ram_style="m10k_mixed" *) reg [UNIT-1:0] mem [0:1023];
 integer i, w, r;
 initial for(i=0;i<1024;i=i+1) mem[i]=(i*73)^(i>>1)^'ha6;
 reg armed=0, we=0;
 reg [WA-1:0] wa=0;
 reg [UNIT-1:0] seed=0;
 reg [UNIT*RLANES-1:0] q;
 always @(posedge FPGA_CLK1_50) begin
  if(gp_out==32'h13579bdf) armed<=1;
  we<=armed && gp_out[31]; wa<=gp_out[WA-1:0]; seed<=gp_out[16+:UNIT];

 end
 wire [RA-1:0] ra=gp_out[RA-1:0];
 genvar lane;
 generate for(lane=0;lane<WLANES;lane=lane+1) begin: write_lane
  if(WLANES==1) begin
   always @(posedge FPGA_CLK1_50) if(we) mem[wa]<=seed;
  end else begin
   localparam [$clog2(WLANES)-1:0] INDEX=lane;
   always @(posedge FPGA_CLK1_50) if(we) mem[{wa,INDEX}]<=seed^(lane*'h93);
  end
 end
 for(lane=0;lane<RLANES;lane=lane+1) begin: read_lane
  if(RLANES==1) begin
   always @(posedge read_clock) if(gp_out[30]) q<=mem[ra];
  end else begin
   localparam [$clog2(RLANES)-1:0] INDEX=lane;
   always @(posedge read_clock) if(gp_out[30]) q[lane*UNIT+:UNIT]<=mem[{ra,INDEX}];
  end
 end endgenerate
 reg [15:0] sampled=0;
 always @(posedge FPGA_CLK1_50) sampled<=q >> {gp_out[27:26],4'b0};
 assign gp_in={16'hd713,sampled};
 cyclonev_hps_interface_mpu_general_purpose hps_gp(.gp_in(gp_in),.gp_out(gp_out));
endmodule
