module top(input wire CLK_W21, output wire LED);
    wire [3:0] clk;
    wire gated, locked;
    wire [31:0] gp_out;
    reg [3:0] c0 = 0, c1 = 0, c2 = 0, c3 = 0, cg = 0;
    always @(posedge clk[0]) c0 <= c0 + 1'b1;
    always @(posedge clk[1]) c1 <= c1 + 1'b1;
    always @(posedge clk[2]) c2 <= c2 + 1'b1;
    always @(posedge clk[3]) c3 <= c3 + 1'b1;
    always @(posedge gated) cg <= cg + 1'b1;
    altera_pll #(
        .reference_clock_frequency("50.0 MHz"), .operation_mode("direct"),
        .number_of_clocks(4), .fractional_vco_multiplier("false"),
        .output_clock_frequency0("100.0 MHz"), .phase_shift0("0 ps"), .duty_cycle0(50),
        .output_clock_frequency1("50.0 MHz"), .phase_shift1("0 ps"), .duty_cycle1(50),
        .output_clock_frequency2("25.0 MHz"), .phase_shift2("0 ps"), .duty_cycle2(50),
        .output_clock_frequency3("20.0 MHz"), .phase_shift3("0 ps"), .duty_cycle3(50)
    ) pll (.refclk(CLK_W21), .rst(1'b0), .outclk(clk), .locked(locked));
    cyclonev_clkena #(.clock_type("Global Clock"), .ena_register_mode("falling edge"),
                      .ena_register_power_up("high"))
        gate (.inclk(clk[0]), .ena(gp_out[0]), .outclk(gated), .enaout());
    cyclonev_hps_interface_mpu_general_purpose hps_gp (
        .gp_in({11'b0, locked, cg, c3, c2, c1, c0}), .gp_out(gp_out));
    assign LED = c0[0] ^ c1[0] ^ c2[0] ^ c3[0] ^ cg[0] ^ locked;
endmodule
