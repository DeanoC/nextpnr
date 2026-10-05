module top(input wire [0:0] refs, input wire [0:0] rsts, output wire out);
    wire [8:0] pll0_clk;
    wire pll0_locked;
    altera_pll #(
        .reference_clock_frequency("50.0 MHz"),
        .number_of_clocks(9),
        .operation_mode("direct"),
        .fractional_vco_multiplier("false"),
        .output_clock_frequency0("150.0 MHz"),
        .phase_shift0("0 ps"),
        .duty_cycle0(50),
        .output_clock_frequency1("120.0 MHz"),
        .phase_shift1("0 ps"),
        .duty_cycle1(50),
        .output_clock_frequency2("100.0 MHz"),
        .phase_shift2("0 ps"),
        .duty_cycle2(50),
        .output_clock_frequency3("75.0 MHz"),
        .phase_shift3("0 ps"),
        .duty_cycle3(50),
        .output_clock_frequency4("60.0 MHz"),
        .phase_shift4("0 ps"),
        .duty_cycle4(50),
        .output_clock_frequency5("50.0 MHz"),
        .phase_shift5("0 ps"),
        .duty_cycle5(50),
        .output_clock_frequency6("40.0 MHz"),
        .phase_shift6("0 ps"),
        .duty_cycle6(50),
        .output_clock_frequency7("30.0 MHz"),
        .phase_shift7("0 ps"),
        .duty_cycle7(50),
        .output_clock_frequency8("25.0 MHz"),
        .phase_shift8("0 ps"),
        .duty_cycle8(50)
    ) pll0 (.refclk(refs[0]), .rst(rsts[0]), .outclk(pll0_clk), .locked(pll0_locked));
    reg [3:0] pll0_c0 = 0;
    always @(posedge pll0_clk[0]) pll0_c0 <= pll0_c0 + 1'b1;
    reg [3:0] pll0_c1 = 0;
    always @(posedge pll0_clk[1]) pll0_c1 <= pll0_c1 + 1'b1;
    reg [3:0] pll0_c2 = 0;
    always @(posedge pll0_clk[2]) pll0_c2 <= pll0_c2 + 1'b1;
    reg [3:0] pll0_c3 = 0;
    always @(posedge pll0_clk[3]) pll0_c3 <= pll0_c3 + 1'b1;
    reg [3:0] pll0_c4 = 0;
    always @(posedge pll0_clk[4]) pll0_c4 <= pll0_c4 + 1'b1;
    reg [3:0] pll0_c5 = 0;
    always @(posedge pll0_clk[5]) pll0_c5 <= pll0_c5 + 1'b1;
    reg [3:0] pll0_c6 = 0;
    always @(posedge pll0_clk[6]) pll0_c6 <= pll0_c6 + 1'b1;
    reg [3:0] pll0_c7 = 0;
    always @(posedge pll0_clk[7]) pll0_c7 <= pll0_c7 + 1'b1;
    reg [3:0] pll0_c8 = 0;
    always @(posedge pll0_clk[8]) pll0_c8 <= pll0_c8 + 1'b1;
    assign out = pll0_c0[3] ^ pll0_c1[3] ^ pll0_c2[3] ^ pll0_c3[3] ^ pll0_c4[3] ^ pll0_c5[3] ^ pll0_c6[3] ^ pll0_c7[3] ^ pll0_c8[3] ^ pll0_locked;
endmodule
