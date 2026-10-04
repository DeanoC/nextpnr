// SDRAM-style data bus: one OE register for every bit, per-bit data
// registers, active-low asynchronous reset and active-low clock enable.
module top(
    input  wire clk, rst_n, ce_n,
    input  wire [3:0] d,
    inout  wire [3:0] dq,
    output wire [3:0] q
);
    reg dq_oe;
    reg [3:0] dq_out, dq_in;
    wire [3:0] dq_pad;
    always @(posedge clk or negedge rst_n)
        if (!rst_n) begin dq_oe <= 1'b0; dq_out <= 4'b0; dq_in <= 4'b0; end
        else if (!ce_n) begin dq_oe <= d[0] ^ d[3]; dq_out <= d; dq_in <= dq_pad; end
`ifdef QUARTUS
    assign dq = dq_oe ? dq_out : 4'bz;
    assign dq_pad = dq;
`else
    MISTRAL_IO pad[3:0] (.PAD(dq), .I(dq_out), .OE({4{dq_oe}}), .O(dq_pad));
`endif
    assign q = dq_in;
endmodule
