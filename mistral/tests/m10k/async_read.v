// Small M10K fixture for the asynchronous read-port contract.
//
// The direct primitive keeps this test independent of the pending Yosys
// memory_libmap rule.  async_read.py removes B1EN and adds CFG_ASYNC_READ to
// model the JSON that that rule will emit.  nextpnr supplies the physical
// RDEN[0] tie required by the Cyclone V read core.
module top(
    input wire FPGA_CLK1_50,
    output wire [0:0] LED
);
    reg [8:0] addr = 9'b0;
    always @(posedge FPGA_CLK1_50)
        addr <= addr + 9'd1;

    wire [19:0] q;
    reg q_sample;
    MISTRAL_M10K #(.CFG_ABITS(9), .CFG_DBITS(20)) ram(
        .CLK1(FPGA_CLK1_50),
        .A1ADDR(9'b0),
        .A1DATA(20'b0),
        .A1EN(1'b1),
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
