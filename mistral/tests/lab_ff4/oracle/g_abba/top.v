module top(input clka, clkb, input [3:0] d, output [3:0] q);
    reg [3:0] r;
    always @(posedge clka) r[0] <= d[0];
    always @(posedge clkb) r[1] <= d[1];
    always @(posedge clkb) r[2] <= d[2];
    always @(posedge clka) r[3] <= d[3];
    assign q = r;
endmodule
