module top(input wire CLK_D12, input wire CLK_Y15, input wire CLK_E11, output wire LED);
    wire [1:0] pll27_clk;
    wire pll27_locked;
    altera_pll #(.reference_clock_frequency("27.0 MHz"), .operation_mode("direct"), .number_of_clocks(2), .fractional_vco_multiplier("false"), .output_clock_frequency0("74.25 MHz"), .phase_shift0("0 ps"), .duty_cycle0(50), .output_clock_frequency1("148.5 MHz"), .phase_shift1("0 ps"), .duty_cycle1(50))
      pll27 (.refclk(CLK_D12), .rst(1'b0), .outclk(pll27_clk), .locked(pll27_locked));
    reg [3:0] pll27_c0 = 0;
    always @(posedge pll27_clk[0]) pll27_c0 <= pll27_c0 + 1'b1;
    reg [3:0] pll27_c1 = 0;
    always @(posedge pll27_clk[1]) pll27_c1 <= pll27_c1 + 1'b1;
    wire [0:0] pll100_clk;
    wire pll100_locked;
    altera_pll #(.reference_clock_frequency("100.0 MHz"), .operation_mode("direct"), .number_of_clocks(1), .fractional_vco_multiplier("true"), .output_clock_frequency0("27.0 MHz"), .phase_shift0("0 ps"), .duty_cycle0(50))
      pll100 (.refclk(CLK_Y15), .rst(1'b0), .outclk(pll100_clk), .locked(pll100_locked));
    reg [3:0] pll100_c0 = 0;
    always @(posedge pll100_clk[0]) pll100_c0 <= pll100_c0 + 1'b1;
    wire [0:0] pll25_clk;
    wire pll25_locked;
    altera_pll #(.reference_clock_frequency("25.0 MHz"), .operation_mode("direct"), .number_of_clocks(1), .fractional_vco_multiplier("false"), .output_clock_frequency0("250.0 MHz"), .phase_shift0("0 ps"), .duty_cycle0(50))
      pll25 (.refclk(CLK_E11), .rst(1'b0), .outclk(pll25_clk), .locked(pll25_locked));
    reg [3:0] pll25_c0 = 0;
    always @(posedge pll25_clk[0]) pll25_c0 <= pll25_c0 + 1'b1;
    assign LED = pll27_c0[3] ^ pll27_c1[3] ^ pll27_locked ^ pll100_c0[3] ^ pll100_locked ^ pll25_c0[3] ^ pll25_locked;
endmodule
