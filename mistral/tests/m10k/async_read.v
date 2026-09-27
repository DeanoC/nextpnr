// Small M10K fixture for rejected async and accepted synchronous shapes.
// unsupported_async.py edits synthesized JSON to test the unsupported
// CFG_ASYNC_READ and omitted-B1EN spellings at each fixed width.
module top #(
    parameter WIDTH = 20
) (
    input wire FPGA_CLK1_50,
    output wire [0:0] LED
);
    localparam ABITS = WIDTH == 40 ? 8 : WIDTH == 20 ? 9 : 10;
    reg [ABITS-1:0] addr = 0;
    always @(posedge FPGA_CLK1_50)
        addr <= addr + 1'b1;

    wire [WIDTH-1:0] q;
    reg q_sample;
    MISTRAL_M10K #(.CFG_ABITS(ABITS), .CFG_DBITS(WIDTH)) ram(
        .CLK1(FPGA_CLK1_50),
        .A1ADDR({ABITS{1'b0}}),
        .A1DATA({WIDTH{1'b0}}),
        .A1EN(WIDTH == 40 ? 1'b0 : 1'b1),
        .B1ADDR(addr),
        .B1DATA(q),
        .B1EN(1'b1)
    );

    // Capture the asynchronous result so the timing report has a
    // clock-to-clock path through the M10K address-to-data arc.
    always @(posedge FPGA_CLK1_50)
        q_sample <= q[0];

    assign LED[0] = q_sample;
endmodule
