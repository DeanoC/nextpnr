module top(input clka, clkb, input [3:0] d, output [3:0] q);
    reg [3:0] r;
    always @(posedge clka) r[1:0] <= d[1:0];
    always @(posedge clkb) r[3:2] <= d[3:2];
    assign q = r;
endmodule
