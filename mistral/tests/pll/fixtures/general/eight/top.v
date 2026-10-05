module top(input wire CLK_W21, output wire LED);
    wire [7:0] pll8_clk;
    wire pll8_locked;
    altera_pll #(.reference_clock_frequency("50.0 MHz"), .operation_mode("direct"), .number_of_clocks(8), .fractional_vco_multiplier("false"), .output_clock_frequency0("150.0 MHz"), .phase_shift0("0 ps"), .duty_cycle0(50), .output_clock_frequency1("120.0 MHz"), .phase_shift1("0 ps"), .duty_cycle1(50), .output_clock_frequency2("100.0 MHz"), .phase_shift2("0 ps"), .duty_cycle2(50), .output_clock_frequency3("75.0 MHz"), .phase_shift3("0 ps"), .duty_cycle3(50), .output_clock_frequency4("60.0 MHz"), .phase_shift4("0 ps"), .duty_cycle4(50), .output_clock_frequency5("50.0 MHz"), .phase_shift5("0 ps"), .duty_cycle5(50), .output_clock_frequency6("40.0 MHz"), .phase_shift6("0 ps"), .duty_cycle6(50), .output_clock_frequency7("30.0 MHz"), .phase_shift7("0 ps"), .duty_cycle7(50))
      pll8 (.refclk(CLK_W21), .rst(1'b0), .outclk(pll8_clk), .locked(pll8_locked));
    reg [3:0] pll8_c0 = 0;
    always @(posedge pll8_clk[0]) pll8_c0 <= pll8_c0 + 1'b1;
    reg [3:0] pll8_c1 = 0;
    always @(posedge pll8_clk[1]) pll8_c1 <= pll8_c1 + 1'b1;
    reg [3:0] pll8_c2 = 0;
    always @(posedge pll8_clk[2]) pll8_c2 <= pll8_c2 + 1'b1;
    reg [3:0] pll8_c3 = 0;
    always @(posedge pll8_clk[3]) pll8_c3 <= pll8_c3 + 1'b1;
    reg [3:0] pll8_c4 = 0;
    always @(posedge pll8_clk[4]) pll8_c4 <= pll8_c4 + 1'b1;
    reg [3:0] pll8_c5 = 0;
    always @(posedge pll8_clk[5]) pll8_c5 <= pll8_c5 + 1'b1;
    reg [3:0] pll8_c6 = 0;
    always @(posedge pll8_clk[6]) pll8_c6 <= pll8_c6 + 1'b1;
    reg [3:0] pll8_c7 = 0;
    always @(posedge pll8_clk[7]) pll8_c7 <= pll8_c7 + 1'b1;
    assign LED = pll8_c0[3] ^ pll8_c1[3] ^ pll8_c2[3] ^ pll8_c3[3] ^ pll8_c4[3] ^ pll8_c5[3] ^ pll8_c6[3] ^ pll8_c7[3] ^ pll8_locked;
endmodule
