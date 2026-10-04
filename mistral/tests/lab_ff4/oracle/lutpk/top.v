module top(input clk, input a, b, c, d, e, h, output [3:0] q);
    (* keep = 1 *) wire f = a & b;
    (* keep = 1 *) wire g = d ^ e;
    reg r0, r1, r2, r3;
    always @(posedge clk) begin r0 <= f; r1 <= c; r2 <= g; r3 <= h; end
    assign q = {r3, r2, r1, r0};
endmodule
