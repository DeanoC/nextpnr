// Direct primitive fixture for a true-dual-port M10K with flow-through reads.
// Each port may still write on its own clock; both addresses feed their read
// outputs without a read clock edge.
module top(
    input wire FPGA_CLK1_50,
    output wire [0:0] LED
);
    wire [31:0] gp_in;
    wire [31:0] gp_out;
    wire clock_b;
    wire pll_locked;
    altera_pll #(
        .reference_clock_frequency("50.0 MHz"),
        .number_of_clocks(1),
        .output_clock_frequency0("25.0 MHz"),
        .phase_shift0("0 ps"),
        .duty_cycle0(50),
        .operation_mode("direct"),
        .fractional_vco_multiplier("false")
    ) pll (
        .refclk(FPGA_CLK1_50),
        .rst(1'b0),
        .outclk(clock_b),
        .locked(pll_locked)
    );
    reg [9:0] address_a = 10'd0;
    reg [9:0] address_b = 10'd1;
    // Keep the HPS GP cell in the fixture while the address registers provide
    // clocked timing startpoints for both asynchronous read paths.
    wire [9:0] data_a = 10'h155;
    wire [9:0] data_b = 10'h2aa;
    wire [9:0] q_a;
    wire [9:0] q_b;
    reg q_a_sample;
    reg q_b_sample;

    MISTRAL_M10K_TDP #(
        .CFG_ABITS(10),
        .CFG_DBITS(10),
        .CFG_ASYNC_READ(1),
        .INIT(10240'h3c5a)
    ) ram (
        .CLK1(FPGA_CLK1_50),
        .CLK2(FPGA_CLK1_50),
        .A1ADDR(address_a),
        .B1ADDR(address_b),
        .A1DATA(data_a),
        .B1DATA(data_b),
        .A1EN(gp_out[28]),
        .B1EN(gp_out[29]),
        .A1WE(gp_out[30]),
        .B1WE(gp_out[31]),
        .A1Q(q_a),
        .B1Q(q_b),
        .ACLR0(1'b0),
        .ACLR1(1'b0)
    );

    // Capture both flow-through outputs so the timing report must include
    // address-to-Q arcs for each physical port.
    always @(posedge FPGA_CLK1_50) begin
        q_a_sample <= q_a[0];
        address_a <= address_a + 10'd1;
    end

    always @(posedge clock_b) begin
        q_b_sample <= q_b[0];
        address_b <= address_b + 10'd1;
    end

    assign gp_in = {16'hd7a2, 14'b0, q_b_sample, q_a_sample};
    assign LED[0] = q_a_sample;
    cyclonev_hps_interface_mpu_general_purpose hps_gp(.gp_in(gp_in), .gp_out(gp_out));
endmodule
