module top(input wire FPGA_CLK1_50, input wire FPGA_CLK2_50, input wire FPGA_CLK3_50, output wire LED);
    wire hdmi_clk, audio_clk, hdmi_locked, audio_locked, core_locked;
    wire [2:0] core_clk;
    altera_pll #(.reference_clock_frequency("50.0 MHz"), .operation_mode("direct"), .number_of_clocks(1),
        .output_clock_frequency0("148.500000 MHz"), .phase_shift0("0 ps"), .duty_cycle0(50),
        .fractional_vco_multiplier("true"), .pll_type("General"), .pll_subtype("General"),
        .output_clock_frequency1("0 MHz"), .phase_shift1("0 ps"), .duty_cycle1(50))
      pll_hdmi (.refclk(FPGA_CLK1_50), .rst(1'b0), .outclk(hdmi_clk), .locked(hdmi_locked));
    altera_pll #(.reference_clock_frequency("50.0 MHz"), .operation_mode("direct"), .number_of_clocks(1),
        .output_clock_frequency0("24.576000 MHz"), .phase_shift0("0 ps"), .duty_cycle0(50),
        .fractional_vco_multiplier("true"))
      pll_audio (.refclk(FPGA_CLK3_50), .rst(1'b0), .outclk(audio_clk), .locked(audio_locked));
    altera_pll #(.reference_clock_frequency("50.0 MHz"), .operation_mode("direct"), .number_of_clocks(3),
        .output_clock_frequency0("189.0 MHz"), .output_clock_frequency1("85.909090 MHz"), .output_clock_frequency2("21.477272 MHz"),
        .phase_shift0("0 ps"), .phase_shift1("0 ps"), .phase_shift2("0 ps"), .duty_cycle0(50), .duty_cycle1(50), .duty_cycle2(50),
        .fractional_vco_multiplier("false"))
      pll_core (.refclk(FPGA_CLK2_50), .rst(1'b0), .outclk(core_clk), .locked(core_locked));
    reg [3:0] a = 0, b = 0, c0 = 0, c1 = 0, c2 = 0;
    always @(posedge hdmi_clk) a <= a + 1'b1;
    always @(posedge audio_clk) b <= b + 1'b1;
    always @(posedge core_clk[0]) c0 <= c0 + 1'b1;
    always @(posedge core_clk[1]) c1 <= c1 + 1'b1;
    always @(posedge core_clk[2]) c2 <= c2 + 1'b1;
    assign LED = a[3] ^ b[3] ^ c0[3] ^ c1[3] ^ c2[3] ^ hdmi_locked ^ audio_locked ^ core_locked;
endmodule
