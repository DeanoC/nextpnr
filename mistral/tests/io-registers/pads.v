// One pad per I/O register combination. Quartus infers the tri-state pads;
// the open flow instantiates MISTRAL_IO.
module top(
    input  wire clk,
    input  wire [7:0] d,
    input  wire ce, rst_a,
    inout  wire p1, p2, p3,
    output wire p4, p6, p8,
    input  wire p5, p7,
    output wire [4:0] q
);
    reg o1; always @(posedge clk) o1 <= d[0];                  // output register, comb OE
    reg e2; always @(posedge clk) e2 <= d[2];                  // OE register, comb data
    reg i3; always @(posedge clk) i3 <= p3_in;                 // input register only
    reg o4; always @(posedge clk) if (ce) o4 <= d[6];          // output-only register, clock enable
    reg i5; always @(posedge clk) if (ce) i5 <= p5;            // input-only register, clock enable
    reg o6; always @(posedge clk or posedge rst_a) if (rst_a) o6 <= 1'b0; else o6 <= d[7];
    reg i7; always @(posedge clk or posedge rst_a) if (rst_a) i7 <= 1'b0; else i7 <= p7;
    reg o8, e8; always @(posedge clk) begin o8 <= d[0] ^ d[7]; e8 <= d[1] ^ d[6]; end
    wire p1_in, p2_in, p3_in;
`ifdef QUARTUS
    assign p1 = d[1] ? o1 : 1'bz;
    assign p2 = e2 ? d[3] : 1'bz;
    assign p3 = d[4] ? d[5] : 1'bz;
    assign p8 = e8 ? o8 : 1'bz;
    assign p1_in = p1;
    assign p2_in = p2;
    assign p3_in = p3;
`else
    MISTRAL_IO p1_pad (.PAD(p1), .I(o1), .OE(d[1]), .O(p1_in));
    MISTRAL_IO p2_pad (.PAD(p2), .I(d[3]), .OE(e2), .O(p2_in));
    MISTRAL_IO p3_pad (.PAD(p3), .I(d[5]), .OE(d[4]), .O(p3_in));
    MISTRAL_IO p8_pad (.PAD(p8), .I(o8), .OE(e8));
`endif
    assign p4 = o4;
    assign p6 = o6;
    assign q = {i3, i5, i7, p1_in, p2_in};
endmodule
