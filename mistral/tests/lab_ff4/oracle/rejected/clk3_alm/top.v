module top(input clka, clkb, clkc, input [2:0] d, output [2:0] q);
    reg [2:0] r;
    always @(posedge clka) r[0] <= d[0];
    always @(posedge clkb) r[1] <= d[1];
    always @(posedge clkc) r[2] <= d[2];
    assign q = r;
endmodule
