module top(input clk, input a, b, c, z, output [1:0] q, output y);
    (* keep = 1 *) wire f = a & b;
    (* keep = 1 *) wire g = f ^ z;
    reg r0, r1;
    always @(posedge clk) begin r0 <= f; r1 <= c; end
    assign q = {r1, r0};
    assign y = g;
endmodule
