// Mixed TDP: addr[9:0], B clock gate[14], seed[25:16], select B[26],
// output window[27], enables A/B[28/29], write enables A/B[30/31].
module top #(parameter UNIT=10, ALANES=2, BLANES=1, SAME_CLOCK=0, RAW=0)(input FPGA_CLK1_50);
 localparam AA=10-$clog2(ALANES), BA=10-$clog2(BLANES);
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
 reg [AA-1:0] addr_a=0;
 reg [BA-1:0] addr_b=0;
 reg [UNIT-1:0] seed_a=0, seed_b=0;
 always @(posedge FPGA_CLK1_50) begin
  if(gp_out==32'h13579bdf) armed<=1;
  if(!gp_out[26]) begin addr_a<=gp_out[AA-1:0]; seed_a<=gp_out[16+:UNIT]; end
  else begin addr_b<=gp_out[BA-1:0]; seed_b<=gp_out[16+:UNIT]; end
 end
 wire we_a=armed && gp_out[30], we_b=armed && gp_out[31];
 wire [UNIT*ALANES-1:0] data_a, q_a;
 wire [UNIT*BLANES-1:0] data_b, q_b;
 genvar lane;
 generate for(lane=0;lane<ALANES;lane=lane+1) begin
  assign data_a[lane*UNIT+:UNIT]=seed_a^(lane*'h93);
 end
 for(lane=0;lane<BLANES;lane=lane+1) begin
  assign data_b[lane*UNIT+:UNIT]=seed_b^((lane+2)*'h93);
 end
 if(RAW) begin: raw
  function [10239:0] initial_words;
   integer a;
   reg [UNIT-1:0] word;
   begin
    initial_words=0;
    for(a=0;a<1024;a=a+1) begin
     word=(a*73)^(a>>1)^'ha6;
     initial_words[a*10+:UNIT]=word;
    end
   end
  endfunction
  wire [10*ALANES-1:0] physical_a, result_a;
  wire [10*BLANES-1:0] physical_b, result_b;
  for(lane=0;lane<ALANES;lane=lane+1) begin
   assign physical_a[lane*10+:10]={{(10-UNIT){1'b0}},data_a[lane*UNIT+:UNIT]};
   assign q_a[lane*UNIT+:UNIT]=result_a[lane*10+:UNIT];
  end
  for(lane=0;lane<BLANES;lane=lane+1) begin
   assign physical_b[lane*10+:10]={{(10-UNIT){1'b0}},data_b[lane*UNIT+:UNIT]};
   assign q_b[lane*UNIT+:UNIT]=result_b[lane*10+:UNIT];
  end
  MISTRAL_M10K_TDP #(.CFG_ABITS(AA), .CFG_DBITS(10*ALANES),
   .CFG_RD_ABITS(BA), .CFG_RD_DBITS(10*BLANES), .CFG_MIXED_WIDTH(1), .INIT(initial_words())) ram(
   .CLK1(FPGA_CLK1_50), .CLK2(clock_b), .A1ADDR(addr_a), .B1ADDR(addr_b),
   .A1DATA(physical_a), .B1DATA(physical_b), .A1Q(result_a), .B1Q(result_b),
   .A1EN(gp_out[28]), .B1EN(gp_out[29]), .A1WE(we_a), .B1WE(we_b));
 end else begin: inferred
  (* ram_style="m10k_tdp_mixed" *) reg [UNIT-1:0] mem [0:1023];
  reg [UNIT*ALANES-1:0] read_a;
  reg [UNIT*BLANES-1:0] read_b;
  integer i;
  initial for(i=0;i<1024;i=i+1) mem[i]=(i*73)^(i>>1)^'ha6;
  for(lane=0;lane<ALANES;lane=lane+1) begin: a_lane
   if(ALANES==1) begin
    always @(posedge FPGA_CLK1_50) if(gp_out[28]) begin
     if(we_a) begin mem[addr_a]<=data_a;read_a<=data_a;end
     else read_a<=mem[addr_a];
    end
   end else begin
    localparam [$clog2(ALANES)-1:0] INDEX=lane;
    always @(posedge FPGA_CLK1_50) if(gp_out[28]) begin
     if(we_a) begin mem[{addr_a,INDEX}]<=data_a[lane*UNIT+:UNIT];read_a[lane*UNIT+:UNIT]<=data_a[lane*UNIT+:UNIT];end
     else read_a[lane*UNIT+:UNIT]<=mem[{addr_a,INDEX}];
    end
   end
  end
  for(lane=0;lane<BLANES;lane=lane+1) begin: b_lane
   if(BLANES==1) begin
    always @(posedge clock_b) if(gp_out[29]) begin
     if(we_b) begin mem[addr_b]<=data_b;read_b<=data_b;end
     else read_b<=mem[addr_b];
    end
   end else begin
    localparam [$clog2(BLANES)-1:0] INDEX=lane;
    always @(posedge clock_b) if(gp_out[29]) begin
     if(we_b) begin mem[{addr_b,INDEX}]<=data_b[lane*UNIT+:UNIT];read_b[lane*UNIT+:UNIT]<=data_b[lane*UNIT+:UNIT];end
     else read_b[lane*UNIT+:UNIT]<=mem[{addr_b,INDEX}];
    end
   end
  end
  assign q_a=read_a;
  assign q_b=read_b;
 end endgenerate
 wire [19:0] selected_q=gp_out[26]?q_b:q_a;
 reg [15:0] sampled=0;
 always @(posedge FPGA_CLK1_50) sampled<=selected_q >> {gp_out[27],4'b0};
 assign gp_in={16'hd716,sampled};
 cyclonev_hps_interface_mpu_general_purpose hps_gp(.gp_in(gp_in), .gp_out(gp_out));
endmodule
