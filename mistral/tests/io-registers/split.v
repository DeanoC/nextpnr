// One bidirectional pad with an input register and an output register.
// Synthesised with -noclkbuf so both clocks are still unbuffered at packing.
module top(
    input  wire clk_a,
    input  wire clk_b,
    input  wire d,
    input  wire oe,
    inout  wire p,
    output wire q
);
    reg o;
    reg i;
    always @(posedge clk_a) o <= d;
    always @(posedge clk_b) i <= p_in;
    wire p_in;
    MISTRAL_IO pad (.PAD(p), .I(o), .OE(oe), .O(p_in));
    assign q = i;
endmodule
