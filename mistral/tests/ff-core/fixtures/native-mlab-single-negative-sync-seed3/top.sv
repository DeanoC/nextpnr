module top(input wire FPGA_CLK1_50,input wire [119:0] din,input wire [8:0] enable,input wire [2:0] sclear,output wire [119:0] probes,output wire [15:0] clock_probe);
    wire clk, clk_memory, clk_capture, clk_pixel, memory_locked, video_locked;
    wire [2:0] clocks;
    altera_pll #(
        .reference_clock_frequency("50.0 MHz"), .number_of_clocks(3),
        .output_clock_frequency0("100.0 MHz"), .output_clock_frequency1("50.0 MHz"), .output_clock_frequency2("50.0 MHz"),
        .phase_shift0("0 ps"), .phase_shift1("10000 ps"), .phase_shift2("0 ps"),
        .duty_cycle0(50), .duty_cycle1(50), .duty_cycle2(50), .operation_mode("direct"),
        .fractional_vco_multiplier("false")
    ) memory_clock (.refclk(FPGA_CLK1_50), .rst(1'b0), .outclk(clocks), .locked(memory_locked));
    assign clk = clocks[0];
    assign clk_memory = clocks[2];
    assign clk_capture = clocks[1];
    altera_pll #(
        .reference_clock_frequency("50.0 MHz"), .number_of_clocks(1),
        .output_clock_frequency0("74.25 MHz"), .phase_shift0("0 ps"), .duty_cycle0(50),
        .operation_mode("direct"), .fractional_vco_multiplier("true")
    ) video_clock (.refclk(FPGA_CLK1_50), .rst(1'b0), .outclk(clk_pixel), .locked(video_locked));



(* preserve *) reg [7:0] fast_counter=0;
always @(posedge clk) fast_counter <= fast_counter+1'b1;
(* preserve *) reg [7:0] pixel_counter=0;
always @(posedge clk_pixel) pixel_counter <= pixel_counter+1'b1;
assign clock_probe={fast_counter,pixel_counter};
(* preserve *) reg [2:0] count=0;
(* preserve *) reg reset_n=1;
always @(posedge clk_memory) begin count<=count+1'b1; reset_n<=!reset_n; end
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_0_0 (.clk(~clk),.d(din[0]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[0]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_0_1 (.clk(~clk),.d(din[1]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[1]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_0_2 (.clk(~clk),.d(din[2]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[2]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_0_3 (.clk(~clk),.d(din[3]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[3]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_1_0 (.clk(~clk),.d(din[4]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[4]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_1_1 (.clk(~clk),.d(din[5]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[5]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_1_2 (.clk(~clk),.d(din[6]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[6]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_1_3 (.clk(~clk),.d(din[7]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[7]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_2_0 (.clk(~clk),.d(din[8]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[8]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_2_1 (.clk(~clk),.d(din[9]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[9]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_2_2 (.clk(~clk),.d(din[10]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[10]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_2_3 (.clk(~clk),.d(din[11]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[11]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_3_0 (.clk(~clk),.d(din[12]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[12]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_3_1 (.clk(~clk),.d(din[13]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[13]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_3_2 (.clk(~clk),.d(din[14]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[14]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_3_3 (.clk(~clk),.d(din[15]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[15]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_4_0 (.clk(~clk),.d(din[16]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[16]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_4_1 (.clk(~clk),.d(din[17]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[17]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_4_2 (.clk(~clk),.d(din[18]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[18]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_4_3 (.clk(~clk),.d(din[19]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[19]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_5_0 (.clk(~clk),.d(din[20]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[20]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_5_1 (.clk(~clk),.d(din[21]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[21]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_5_2 (.clk(~clk),.d(din[22]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[22]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_5_3 (.clk(~clk),.d(din[23]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[23]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_6_0 (.clk(~clk),.d(din[24]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[24]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_6_1 (.clk(~clk),.d(din[25]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[25]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_6_2 (.clk(~clk),.d(din[26]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[26]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_6_3 (.clk(~clk),.d(din[27]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[27]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_7_0 (.clk(~clk),.d(din[28]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[28]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_7_1 (.clk(~clk),.d(din[29]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[29]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_7_2 (.clk(~clk),.d(din[30]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[30]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_7_3 (.clk(~clk),.d(din[31]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[31]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_8_0 (.clk(~clk),.d(din[32]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[32]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_8_1 (.clk(~clk),.d(din[33]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[33]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_8_2 (.clk(~clk),.d(din[34]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[34]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_8_3 (.clk(~clk),.d(din[35]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[35]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_9_0 (.clk(~clk),.d(din[36]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[36]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_9_1 (.clk(~clk),.d(din[37]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[37]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_9_2 (.clk(~clk),.d(din[38]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[38]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_9_3 (.clk(~clk),.d(din[39]),.clrn(1'b1),.prn(1'b1),.ena(enable[0]),.asdata(1'b0),.sclr(~sclear[0]),.sload(1'b0),.aload(1'b0),.q(probes[39]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_0_0 (.clk(~clk_memory),.d(din[40]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[40]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_0_1 (.clk(~clk_memory),.d(din[41]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[41]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_0_2 (.clk(~clk_memory),.d(din[42]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[42]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_0_3 (.clk(~clk_memory),.d(din[43]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[43]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_1_0 (.clk(~clk_memory),.d(din[44]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[44]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_1_1 (.clk(~clk_memory),.d(din[45]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[45]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_1_2 (.clk(~clk_memory),.d(din[46]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[46]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_1_3 (.clk(~clk_memory),.d(din[47]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[47]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_2_0 (.clk(~clk_memory),.d(din[48]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[48]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_2_1 (.clk(~clk_memory),.d(din[49]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[49]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_2_2 (.clk(~clk_memory),.d(din[50]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[50]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_2_3 (.clk(~clk_memory),.d(din[51]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[51]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_3_0 (.clk(~clk_memory),.d(din[52]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[52]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_3_1 (.clk(~clk_memory),.d(din[53]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[53]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_3_2 (.clk(~clk_memory),.d(din[54]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[54]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_3_3 (.clk(~clk_memory),.d(din[55]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[55]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_4_0 (.clk(~clk_memory),.d(din[56]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[56]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_4_1 (.clk(~clk_memory),.d(din[57]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[57]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_4_2 (.clk(~clk_memory),.d(din[58]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[58]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_4_3 (.clk(~clk_memory),.d(din[59]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[59]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_5_0 (.clk(~clk_memory),.d(din[60]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[60]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_5_1 (.clk(~clk_memory),.d(din[61]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[61]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_5_2 (.clk(~clk_memory),.d(din[62]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[62]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_5_3 (.clk(~clk_memory),.d(din[63]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[63]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_6_0 (.clk(~clk_memory),.d(din[64]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[64]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_6_1 (.clk(~clk_memory),.d(din[65]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[65]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_6_2 (.clk(~clk_memory),.d(din[66]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[66]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_6_3 (.clk(~clk_memory),.d(din[67]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[67]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_7_0 (.clk(~clk_memory),.d(din[68]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[68]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_7_1 (.clk(~clk_memory),.d(din[69]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[69]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_7_2 (.clk(~clk_memory),.d(din[70]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[70]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_7_3 (.clk(~clk_memory),.d(din[71]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[71]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_8_0 (.clk(~clk_memory),.d(din[72]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[72]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_8_1 (.clk(~clk_memory),.d(din[73]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[73]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_8_2 (.clk(~clk_memory),.d(din[74]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[74]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_8_3 (.clk(~clk_memory),.d(din[75]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[75]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_9_0 (.clk(~clk_memory),.d(din[76]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[76]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_9_1 (.clk(~clk_memory),.d(din[77]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[77]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_9_2 (.clk(~clk_memory),.d(din[78]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[78]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_9_3 (.clk(~clk_memory),.d(din[79]),.clrn(1'b1),.prn(1'b1),.ena(enable[3]),.asdata(1'b0),.sclr(~sclear[1]),.sload(1'b0),.aload(1'b0),.q(probes[79]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_0_0 (.clk(~clk_pixel),.d(din[80]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[80]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_0_1 (.clk(~clk_pixel),.d(din[81]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[81]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_0_2 (.clk(~clk_pixel),.d(din[82]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[82]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_0_3 (.clk(~clk_pixel),.d(din[83]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[83]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_1_0 (.clk(~clk_pixel),.d(din[84]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[84]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_1_1 (.clk(~clk_pixel),.d(din[85]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[85]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_1_2 (.clk(~clk_pixel),.d(din[86]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[86]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_1_3 (.clk(~clk_pixel),.d(din[87]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[87]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_2_0 (.clk(~clk_pixel),.d(din[88]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[88]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_2_1 (.clk(~clk_pixel),.d(din[89]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[89]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_2_2 (.clk(~clk_pixel),.d(din[90]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[90]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_2_3 (.clk(~clk_pixel),.d(din[91]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[91]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_3_0 (.clk(~clk_pixel),.d(din[92]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[92]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_3_1 (.clk(~clk_pixel),.d(din[93]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[93]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_3_2 (.clk(~clk_pixel),.d(din[94]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[94]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_3_3 (.clk(~clk_pixel),.d(din[95]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[95]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_4_0 (.clk(~clk_pixel),.d(din[96]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[96]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_4_1 (.clk(~clk_pixel),.d(din[97]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[97]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_4_2 (.clk(~clk_pixel),.d(din[98]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[98]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_4_3 (.clk(~clk_pixel),.d(din[99]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[99]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_5_0 (.clk(~clk_pixel),.d(din[100]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[100]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_5_1 (.clk(~clk_pixel),.d(din[101]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[101]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_5_2 (.clk(~clk_pixel),.d(din[102]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[102]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_5_3 (.clk(~clk_pixel),.d(din[103]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[103]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_6_0 (.clk(~clk_pixel),.d(din[104]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[104]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_6_1 (.clk(~clk_pixel),.d(din[105]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[105]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_6_2 (.clk(~clk_pixel),.d(din[106]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[106]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_6_3 (.clk(~clk_pixel),.d(din[107]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[107]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_7_0 (.clk(~clk_pixel),.d(din[108]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[108]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_7_1 (.clk(~clk_pixel),.d(din[109]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[109]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_7_2 (.clk(~clk_pixel),.d(din[110]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[110]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_7_3 (.clk(~clk_pixel),.d(din[111]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[111]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_8_0 (.clk(~clk_pixel),.d(din[112]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[112]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_8_1 (.clk(~clk_pixel),.d(din[113]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[113]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_8_2 (.clk(~clk_pixel),.d(din[114]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[114]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_8_3 (.clk(~clk_pixel),.d(din[115]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[115]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_9_0 (.clk(~clk_pixel),.d(din[116]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[116]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_9_1 (.clk(~clk_pixel),.d(din[117]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[117]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_9_2 (.clk(~clk_pixel),.d(din[118]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[118]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_9_3 (.clk(~clk_pixel),.d(din[119]),.clrn(1'b1),.prn(1'b1),.ena(enable[6]),.asdata(1'b0),.sclr(~sclear[2]),.sload(1'b0),.aload(1'b0),.q(probes[119]));
endmodule
