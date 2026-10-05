module top(input clka, clkb, input e0, e1, e2, input [5:0] d, output [5:0] q);
    reg [5:0] r;
    always @(posedge clka) if (e0) r[1:0] <= d[1:0];
    always @(posedge clka) if (e1) r[3:2] <= d[3:2];
    always @(posedge clkb) if (e2) r[5:4] <= d[5:4];
    assign q = r;
endmodule
