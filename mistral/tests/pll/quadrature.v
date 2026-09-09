// All ordered quadrature clock crossings, with rising and falling capture edges.
module top(input wire FPGA_CLK1_50);
    wire [3:0] clocks;
    wire locked;
    wire [31:0] gpo;
    reg launch0 = 0, capture0 = 0;
    always @(posedge clocks[0]) launch0 <= gpo[0];
    always @(posedge clocks[1]) capture0 <= launch0;
    reg launch1 = 0, capture1 = 0;
    always @(posedge clocks[0]) launch1 <= gpo[1];
    always @(negedge clocks[1]) capture1 <= launch1;
    reg launch2 = 0, capture2 = 0;
    always @(posedge clocks[0]) launch2 <= gpo[2];
    always @(posedge clocks[2]) capture2 <= launch2;
    reg launch3 = 0, capture3 = 0;
    always @(posedge clocks[0]) launch3 <= gpo[3];
    always @(negedge clocks[2]) capture3 <= launch3;
    reg launch4 = 0, capture4 = 0;
    always @(posedge clocks[0]) launch4 <= gpo[4];
    always @(posedge clocks[3]) capture4 <= launch4;
    reg launch5 = 0, capture5 = 0;
    always @(posedge clocks[0]) launch5 <= gpo[5];
    always @(negedge clocks[3]) capture5 <= launch5;
    reg launch6 = 0, capture6 = 0;
    always @(posedge clocks[1]) launch6 <= gpo[6];
    always @(posedge clocks[0]) capture6 <= launch6;
    reg launch7 = 0, capture7 = 0;
    always @(posedge clocks[1]) launch7 <= gpo[7];
    always @(negedge clocks[0]) capture7 <= launch7;
    reg launch8 = 0, capture8 = 0;
    always @(posedge clocks[1]) launch8 <= gpo[8];
    always @(posedge clocks[2]) capture8 <= launch8;
    reg launch9 = 0, capture9 = 0;
    always @(posedge clocks[1]) launch9 <= gpo[9];
    always @(negedge clocks[2]) capture9 <= launch9;
    reg launch10 = 0, capture10 = 0;
    always @(posedge clocks[1]) launch10 <= gpo[10];
    always @(posedge clocks[3]) capture10 <= launch10;
    reg launch11 = 0, capture11 = 0;
    always @(posedge clocks[1]) launch11 <= gpo[11];
    always @(negedge clocks[3]) capture11 <= launch11;
    reg launch12 = 0, capture12 = 0;
    always @(posedge clocks[2]) launch12 <= gpo[12];
    always @(posedge clocks[0]) capture12 <= launch12;
    reg launch13 = 0, capture13 = 0;
    always @(posedge clocks[2]) launch13 <= gpo[13];
    always @(negedge clocks[0]) capture13 <= launch13;
    reg launch14 = 0, capture14 = 0;
    always @(posedge clocks[2]) launch14 <= gpo[14];
    always @(posedge clocks[1]) capture14 <= launch14;
    reg launch15 = 0, capture15 = 0;
    always @(posedge clocks[2]) launch15 <= gpo[15];
    always @(negedge clocks[1]) capture15 <= launch15;
    reg launch16 = 0, capture16 = 0;
    always @(posedge clocks[2]) launch16 <= gpo[16];
    always @(posedge clocks[3]) capture16 <= launch16;
    reg launch17 = 0, capture17 = 0;
    always @(posedge clocks[2]) launch17 <= gpo[17];
    always @(negedge clocks[3]) capture17 <= launch17;
    reg launch18 = 0, capture18 = 0;
    always @(posedge clocks[3]) launch18 <= gpo[18];
    always @(posedge clocks[0]) capture18 <= launch18;
    reg launch19 = 0, capture19 = 0;
    always @(posedge clocks[3]) launch19 <= gpo[19];
    always @(negedge clocks[0]) capture19 <= launch19;
    reg launch20 = 0, capture20 = 0;
    always @(posedge clocks[3]) launch20 <= gpo[20];
    always @(posedge clocks[1]) capture20 <= launch20;
    reg launch21 = 0, capture21 = 0;
    always @(posedge clocks[3]) launch21 <= gpo[21];
    always @(negedge clocks[1]) capture21 <= launch21;
    reg launch22 = 0, capture22 = 0;
    always @(posedge clocks[3]) launch22 <= gpo[22];
    always @(posedge clocks[2]) capture22 <= launch22;
    reg launch23 = 0, capture23 = 0;
    always @(posedge clocks[3]) launch23 <= gpo[23];
    always @(negedge clocks[2]) capture23 <= launch23;
    altera_pll #(
        .reference_clock_frequency("50.0 MHz"), .number_of_clocks(4),
        .output_clock_frequency0("25.0 MHz"), .phase_shift0("0 ps"), .duty_cycle0(50),
        .output_clock_frequency1("25.0 MHz"), .phase_shift1("10000 ps"), .duty_cycle1(50),
        .output_clock_frequency2("25.0 MHz"), .phase_shift2("20000 ps"), .duty_cycle2(50),
        .output_clock_frequency3("25.0 MHz"), .phase_shift3("30000 ps"), .duty_cycle3(50),
        .operation_mode("direct"), .fractional_vco_multiplier("false")
    ) pll (.refclk(FPGA_CLK1_50), .rst(1'b0), .outclk(clocks), .locked(locked));
    cyclonev_hps_interface_mpu_general_purpose hps_gp (
        .gp_in({7'b0, locked, capture23, capture22, capture21, capture20, capture19, capture18, capture17, capture16, capture15, capture14, capture13, capture12, capture11, capture10, capture9, capture8, capture7, capture6, capture5, capture4, capture3, capture2, capture1, capture0}), .gp_out(gpo));
endmodule
