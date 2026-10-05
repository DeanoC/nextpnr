module top(input clk, e0, e1, input [3:0] d, output [3:0] q);
    reg [3:0] r;
    always @(posedge clk) if (e0) r[0] <= d[0];
    always @(posedge clk) if (e0) r[1] <= d[1];
    always @(posedge clk) if (e1) r[2] <= d[2];
    always @(posedge clk) if (e1) r[3] <= d[3];
    assign q = r;
endmodule
