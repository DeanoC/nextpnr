module top(input wire FPGA_CLK1_50, output wire LED);
    wire clk25;
    wire locked;
    altera_pll #(
        .reference_clock_frequency("50.0 MHz"),
        .number_of_clocks(1),
        .output_clock_frequency0("25.0 MHz"),
        .phase_shift0("0 ps"),
        .duty_cycle0(50),
        .operation_mode("direct"),
        .fractional_vco_multiplier("false")
    ) pll (
        .refclk(FPGA_CLK1_50), .rst(1'b0),
        .outclk(clk25), .locked(locked)
    );
    reg [23:0] count = 0;
    always @(posedge clk25)
        count <= count + 1'b1;
    assign LED = count[23] & locked;
endmodule
