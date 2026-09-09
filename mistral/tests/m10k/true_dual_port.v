// True dual-port diagnostic. GPO addr[9:0], clock-B gate[14], seed[25:16],
// select B[26], read window[27], enable A/B[28/29], write A/B[30/31].
module top #(parameter WIDTH=20, SAME_CLOCK=0)(input FPGA_CLK1_50);
 localparam ABITS=(WIDTH>10)?9:10;
 wire [31:0] gp_in, gp_out;
 wire clock_b;
 generate if(SAME_CLOCK) begin
  assign clock_b=FPGA_CLK1_50;
 end else begin
  wire pll_clock, locked;
  altera_pll #(.reference_clock_frequency("50.0 MHz"), .number_of_clocks(1),
   .output_clock_frequency0("25.0 MHz"), .phase_shift0("0 ps"), .duty_cycle0(50),
   .operation_mode("direct"), .fractional_vco_multiplier("false"))
   pll(.refclk(FPGA_CLK1_50), .rst(1'b0), .outclk(pll_clock), .locked(locked));
  cyclonev_clkena #(.clock_type("global clock"), .ena_register_mode("falling edge"),
   .ena_register_power_up("high"), .disable_mode("low"), .test_syn("high"))
   gate(.inclk(pll_clock), .ena(gp_out[14]), .outclk(clock_b), .enaout());
 end endgenerate
 reg armed=0;
 reg [ABITS-1:0] addr_a=0, addr_b=0;
 reg [9:0] seed_a=0, seed_b=0;
 always @(posedge FPGA_CLK1_50) begin
  if(gp_out==32'h13579bdf) armed<=1;
  if(!gp_out[26]) begin addr_a<=gp_out[ABITS-1:0]; seed_a<=gp_out[25:16]; end
  else begin addr_b<=gp_out[ABITS-1:0]; seed_b<=gp_out[25:16]; end
 end
 wire [WIDTH-1:0] data_a=seed_a ^ 20'h93a00;
 wire [WIDTH-1:0] data_b=seed_b ^ 20'h2bc00;
 wire we_a=armed && gp_out[30], we_b=armed && gp_out[31];
 (* ram_style="m10k_tdp" *) reg [WIDTH-1:0] mem [0:(1<<ABITS)-1];
 reg [WIDTH-1:0] q_a, q_b;
 integer i;
 initial for(i=0;i<(1<<ABITS);i=i+1) mem[i]=(i*32'h9e3779b9)^32'h0065acbd;
 always @(posedge FPGA_CLK1_50) if(gp_out[28]) begin
  if(we_a) begin mem[addr_a]<=data_a; q_a<=data_a; end
  else q_a<=mem[addr_a];
 end
 always @(posedge clock_b) if(gp_out[29]) begin
  if(we_b) begin mem[addr_b]<=data_b; q_b<=data_b; end
  else q_b<=mem[addr_b];
 end
 wire [WIDTH-1:0] selected_q=gp_out[26]?q_b:q_a;
 reg [15:0] sampled=0;
 always @(posedge FPGA_CLK1_50) sampled<=selected_q >> {gp_out[27],4'b0};
 assign gp_in={16'hd714,sampled};
 cyclonev_hps_interface_mpu_general_purpose hps_gp(.gp_in(gp_in), .gp_out(gp_out));
endmodule
