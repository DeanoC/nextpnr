module top(input clk, input [3:0] d, output [3:0] q);
    reg [3:0] r;
    always @(posedge clk) r[1:0] <= d[1:0];
    always @(negedge clk) r[3:2] <= d[3:2];
    assign q = r;
endmodule
