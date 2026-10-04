module top(input wire [0:0] refs, input wire [0:0] rsts, output wire out);
    wire [0:0] pll0_clk;
    wire pll0_locked;
    altera_pll #(
        .reference_clock_frequency("50.0 MHz"),
        .number_of_clocks(1),
        .operation_mode("direct"),
        .fractional_vco_multiplier("false"),
        .output_clock_frequency0("145.454545 MHz"),
        .phase_shift0("0 ps"),
        .duty_cycle0(50)
    ) pll0 (.refclk(refs[0]), .rst(rsts[0]), .outclk(pll0_clk), .locked(pll0_locked));
    reg [3:0] pll0_c0 = 0;
    always @(posedge pll0_clk[0]) pll0_c0 <= pll0_c0 + 1'b1;
    assign out = pll0_c0[3] ^ pll0_locked;
endmodule
