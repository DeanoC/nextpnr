// Two phase-related PLL clocks (50 MHz at 0 and 90 degrees), a deep path
// inside clk_fast and register paths crossing both ways.
module top(input wire FPGA_CLK1_50, input wire [7:0] D, output wire [3:0] Q);
    wire [1:0] clocks;
    wire clk_fast = clocks[0];
    wire clk_slow = clocks[1];
    wire locked;
    altera_pll #(
        .reference_clock_frequency("50.0 MHz"), .number_of_clocks(2),
        .output_clock_frequency0("50.0 MHz"), .output_clock_frequency1("50.0 MHz"),
        .phase_shift0("0 ps"), .phase_shift1("5000 ps"),
        .duty_cycle0(50), .duty_cycle1(50), .operation_mode("direct"),
        .fractional_vco_multiplier("false")
    ) pll (.refclk(FPGA_CLK1_50), .rst(1'b0), .outclk(clocks), .locked(locked));

    reg [15:0] a, b, m;
    always @(posedge clk_fast) begin
        a <= {a[14:0], D[0]} ^ {D, D};
        b <= {b[14:0], D[1]} ^ {D[3:0], D, D[7:4]};
        m <= (a * b) ^ ((a + b) * (a - b)); // deep LUT logic
    end
    reg [15:0] fast_to_slow, slow_reg, slow_to_fast;
    always @(posedge clk_slow) begin
        fast_to_slow <= m * m;
        slow_reg <= fast_to_slow + slow_reg;
    end
    always @(posedge clk_fast)
        slow_to_fast <= slow_reg * a;
    assign Q = {^m, ^slow_reg, ^slow_to_fast, locked};
endmodule
