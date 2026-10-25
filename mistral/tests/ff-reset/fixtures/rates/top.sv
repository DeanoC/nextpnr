module top(input wire FPGA_CLK1_50, output wire [5:0] probes);
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



(* preserve, dont_merge *) reg [2:0] count_0=0;
(* preserve, dont_merge *) reg reset_n_0=1;
always @(posedge clk) begin
 count_0 <= count_0+1'b1;
 reset_n_0 <= !reset_n_0;
end
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_0 (
 .clk(clk), .d(count_0[1]), .clrn(reset_n_0), .prn(1'b1),
 .ena(1'b1), .asdata(1'b0), .sclr(1'b0), .sload(1'b0), .aload(1'b0),
 .q(probes[0]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_0_1 (
 .clk(clk), .d(count_0[1]), .clrn(reset_n_0), .prn(1'b1),
 .ena(count_0[2]), .asdata(1'b0), .sclr(1'b0), .sload(1'b0), .aload(1'b0),
 .q(probes[1]));

(* preserve, dont_merge *) reg [2:0] count_1=0;
(* preserve, dont_merge *) reg reset_n_1=1;
always @(posedge clk_memory) begin
 count_1 <= count_1+1'b1;
 reset_n_1 <= !reset_n_1;
end
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_0 (
 .clk(clk_memory), .d(count_1[1]), .clrn(reset_n_1), .prn(1'b1),
 .ena(1'b1), .asdata(1'b0), .sclr(1'b0), .sload(1'b0), .aload(1'b0),
 .q(probes[2]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_1_1 (
 .clk(clk_memory), .d(count_1[1]), .clrn(reset_n_1), .prn(1'b1),
 .ena(count_1[2]), .asdata(1'b0), .sclr(1'b0), .sload(1'b0), .aload(1'b0),
 .q(probes[3]));

(* preserve, dont_merge *) reg [2:0] count_2=0;
(* preserve, dont_merge *) reg reset_n_2=1;
always @(posedge clk_pixel) begin
 count_2 <= count_2+1'b1;
 reset_n_2 <= !reset_n_2;
end
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_0 (
 .clk(clk_pixel), .d(count_2[1]), .clrn(reset_n_2), .prn(1'b1),
 .ena(1'b1), .asdata(1'b0), .sclr(1'b0), .sload(1'b0), .aload(1'b0),
 .q(probes[4]));
(* preserve, dont_merge *) dffeas #(.power_up("low")) sink_2_1 (
 .clk(clk_pixel), .d(count_2[1]), .clrn(reset_n_2), .prn(1'b1),
 .ena(count_2[2]), .asdata(1'b0), .sclr(1'b0), .sload(1'b0), .aload(1'b0),
 .q(probes[5]));
endmodule
