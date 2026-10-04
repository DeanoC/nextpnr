module top(input wire [4:0] refs, input wire [4:0] rsts, output wire out);
    wire [1:0] pll0_clk;
    wire pll0_locked;
    altera_pll #(
        .reference_clock_frequency("600.0 MHz"),
        .number_of_clocks(2),
        .operation_mode("direct"),
        .fractional_vco_multiplier("false"),
        .output_clock_frequency0("300.0 MHz"),
        .phase_shift0("0 ps"),
        .duty_cycle0(50),
        .output_clock_frequency1("225.0 MHz"),
        .phase_shift1("0 ps"),
        .duty_cycle1(50)
    ) pll0 (.refclk(refs[0]), .rst(rsts[0]), .outclk(pll0_clk), .locked(pll0_locked));
    reg [3:0] pll0_c0 = 0;
    always @(posedge pll0_clk[0]) pll0_c0 <= pll0_c0 + 1'b1;
    reg [3:0] pll0_c1 = 0;
    always @(posedge pll0_clk[1]) pll0_c1 <= pll0_c1 + 1'b1;
    wire [1:0] pll1_clk;
    wire pll1_locked;
    altera_pll #(
        .reference_clock_frequency("650.0 MHz"),
        .number_of_clocks(2),
        .operation_mode("direct"),
        .fractional_vco_multiplier("false"),
        .output_clock_frequency0("325.0 MHz"),
        .phase_shift0("0 ps"),
        .duty_cycle0(50),
        .output_clock_frequency1("243.75 MHz"),
        .phase_shift1("0 ps"),
        .duty_cycle1(50)
    ) pll1 (.refclk(refs[1]), .rst(rsts[1]), .outclk(pll1_clk), .locked(pll1_locked));
    reg [3:0] pll1_c0 = 0;
    always @(posedge pll1_clk[0]) pll1_c0 <= pll1_c0 + 1'b1;
    reg [3:0] pll1_c1 = 0;
    always @(posedge pll1_clk[1]) pll1_c1 <= pll1_c1 + 1'b1;
    wire [0:0] pll2_clk;
    wire pll2_locked;
    altera_pll #(
        .reference_clock_frequency("700.0 MHz"),
        .number_of_clocks(1),
        .operation_mode("direct"),
        .fractional_vco_multiplier("false"),
        .output_clock_frequency0("175.0 MHz"),
        .phase_shift0("0 ps"),
        .duty_cycle0(50)
    ) pll2 (.refclk(refs[2]), .rst(rsts[2]), .outclk(pll2_clk), .locked(pll2_locked));
    reg [3:0] pll2_c0 = 0;
    always @(posedge pll2_clk[0]) pll2_c0 <= pll2_c0 + 1'b1;
    wire [1:0] pll3_clk;
    wire pll3_locked;
    altera_pll #(
        .reference_clock_frequency("700.0 MHz"),
        .number_of_clocks(2),
        .operation_mode("direct"),
        .fractional_vco_multiplier("false"),
        .output_clock_frequency0("350.0 MHz"),
        .phase_shift0("0 ps"),
        .duty_cycle0(50),
        .output_clock_frequency1("233.333333 MHz"),
        .phase_shift1("0 ps"),
        .duty_cycle1(50)
    ) pll3 (.refclk(refs[3]), .rst(rsts[3]), .outclk(pll3_clk), .locked(pll3_locked));
    reg [3:0] pll3_c0 = 0;
    always @(posedge pll3_clk[0]) pll3_c0 <= pll3_c0 + 1'b1;
    reg [3:0] pll3_c1 = 0;
    always @(posedge pll3_clk[1]) pll3_c1 <= pll3_c1 + 1'b1;
    wire [1:0] pll4_clk;
    wire pll4_locked;
    altera_pll #(
        .reference_clock_frequency("700.0 MHz"),
        .number_of_clocks(2),
        .operation_mode("direct"),
        .fractional_vco_multiplier("false"),
        .output_clock_frequency0("350.0 MHz"),
        .phase_shift0("0 ps"),
        .duty_cycle0(50),
        .output_clock_frequency1("262.5 MHz"),
        .phase_shift1("0 ps"),
        .duty_cycle1(50)
    ) pll4 (.refclk(refs[4]), .rst(rsts[4]), .outclk(pll4_clk), .locked(pll4_locked));
    reg [3:0] pll4_c0 = 0;
    always @(posedge pll4_clk[0]) pll4_c0 <= pll4_c0 + 1'b1;
    reg [3:0] pll4_c1 = 0;
    always @(posedge pll4_clk[1]) pll4_c1 <= pll4_c1 + 1'b1;
    assign out = pll0_c0[3] ^ pll0_c1[3] ^ pll0_locked ^ pll1_c0[3] ^ pll1_c1[3] ^ pll1_locked ^ pll2_c0[3] ^ pll2_locked ^ pll3_c0[3] ^ pll3_c1[3] ^ pll3_locked ^ pll4_c0[3] ^ pll4_c1[3] ^ pll4_locked;
endmodule
