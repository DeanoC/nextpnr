// Byte-masked TDP diagnostic: addr[8:0], masks A[11:10]/B[13:12],
// clock-B gate[14], seed[25:16],
// select B[26], read window[27], enable A/B[28/29], write A/B[30/31].
module top #(parameter WIDTH=20, SAME_CLOCK=0, RAW=0)(input FPGA_CLK1_50);
 localparam ABITS=9;
 localparam LANE=WIDTH/2;
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
 wire [WIDTH-1:0] q_a, q_b;
 generate if(RAW) begin: raw
  function [10239:0] initial_words;
   integer a;
   reg [WIDTH-1:0] word;
   begin
    initial_words=0;
    for(a=0;a<512;a=a+1) begin
     word=(a*32'h9e3779b9)^32'h0065acbd;
     initial_words[a*20 +: LANE]=word[0 +: LANE];
     initial_words[a*20+10 +: LANE]=word[LANE +: LANE];
    end
   end
  endfunction
  wire [19:0] result_a, result_b;
  MISTRAL_M10K_TDP #(.CFG_ABITS(9), .CFG_DBITS(20), .CFG_BYTE_ENABLE(1), .INIT(initial_words())) ram(
   .CLK1(FPGA_CLK1_50), .CLK2(clock_b), .A1ADDR(addr_a), .B1ADDR(addr_b),
   .A1DATA({{(10-LANE){1'b0}},data_a[LANE +: LANE],{(10-LANE){1'b0}},data_a[0 +: LANE]}),
   .B1DATA({{(10-LANE){1'b0}},data_b[LANE +: LANE],{(10-LANE){1'b0}},data_b[0 +: LANE]}),
   .A1EN(gp_out[28]), .B1EN(gp_out[29]), .A1WE(we_a), .B1WE(we_b),
   .A1BE(gp_out[11:10]), .B1BE(gp_out[13:12]), .A1Q(result_a), .B1Q(result_b));
  assign q_a={result_a[10 +: LANE],result_a[0 +: LANE]};
  assign q_b={result_b[10 +: LANE],result_b[0 +: LANE]};
 end else begin: inferred
 (* ram_style="m10k_tdp_byte" *) reg [WIDTH-1:0] mem [0:(1<<ABITS)-1];
 reg [WIDTH-1:0] read_a, read_b;
 integer i;
 initial for(i=0;i<(1<<ABITS);i=i+1) mem[i]=(i*32'h9e3779b9)^32'h0065acbd;
 always @(posedge FPGA_CLK1_50) if(gp_out[28]) begin
  if(we_a) begin
   if(gp_out[10]) begin mem[addr_a][LANE-1:0]<=data_a[LANE-1:0]; end
   if(gp_out[11]) begin mem[addr_a][WIDTH-1:LANE]<=data_a[WIDTH-1:LANE]; end
  end
  else read_a<=mem[addr_a];
 end
 always @(posedge clock_b) if(gp_out[29]) begin
  if(we_b) begin
   if(gp_out[12]) begin mem[addr_b][LANE-1:0]<=data_b[LANE-1:0]; end
   if(gp_out[13]) begin mem[addr_b][WIDTH-1:LANE]<=data_b[WIDTH-1:LANE]; end
  end
  else read_b<=mem[addr_b];
 end
  assign q_a=read_a;
  assign q_b=read_b;
 end endgenerate
 wire [WIDTH-1:0] selected_q=gp_out[26]?q_b:q_a;
 reg [15:0] sampled=0;
 always @(posedge FPGA_CLK1_50) sampled<=selected_q >> {gp_out[27],4'b0};
 assign gp_in={16'hd715,sampled};
 cyclonev_hps_interface_mpu_general_purpose hps_gp(.gp_in(gp_in), .gp_out(gp_out));
endmodule
