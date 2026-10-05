module top(input clk, input [3:0] d, output [3:0] q);
    reg [3:0] r;
    always @(posedge clk) r <= d;
    assign q = r;
endmodule
