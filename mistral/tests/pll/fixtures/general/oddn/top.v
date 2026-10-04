module top(input wire CLK_V12, output wire LED);
    wire [3:0] pllodd_clk;
    wire pllodd_locked;
    altera_pll #(.reference_clock_frequency("50.0 MHz"), .operation_mode("direct"), .number_of_clocks(4), .fractional_vco_multiplier("false"), .output_clock_frequency0("40.0 MHz"), .phase_shift0("0 ps"), .duty_cycle0(50), .output_clock_frequency1("80.0 MHz"), .phase_shift1("3125 ps"), .duty_cycle1(50), .output_clock_frequency2("16.0 MHz"), .phase_shift2("0 ps"), .duty_cycle2(25), .output_clock_frequency3("32.0 MHz"), .phase_shift3("0 ps"), .duty_cycle3(40))
      pllodd (.refclk(CLK_V12), .rst(1'b0), .outclk(pllodd_clk), .locked(pllodd_locked));
    reg [3:0] pllodd_c0 = 0;
    always @(posedge pllodd_clk[0]) pllodd_c0 <= pllodd_c0 + 1'b1;
    reg [3:0] pllodd_c1 = 0;
    always @(posedge pllodd_clk[1]) pllodd_c1 <= pllodd_c1 + 1'b1;
    reg [3:0] pllodd_c2 = 0;
    always @(posedge pllodd_clk[2]) pllodd_c2 <= pllodd_c2 + 1'b1;
    reg [3:0] pllodd_c3 = 0;
    always @(posedge pllodd_clk[3]) pllodd_c3 <= pllodd_c3 + 1'b1;
    assign LED = pllodd_c0[3] ^ pllodd_c1[3] ^ pllodd_c2[3] ^ pllodd_c3[3] ^ pllodd_locked;
endmodule
