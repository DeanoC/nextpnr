module top(input clk, rstg, rstl, input [3:0] d, output [3:0] q);
    reg [3:0] r;
    always @(posedge clk or posedge rstg) if (rstg) r[1:0] <= 2'b0; else r[1:0] <= d[1:0];
    always @(posedge clk or posedge rstl) if (rstl) r[3:2] <= 2'b0; else r[3:2] <= d[3:2];
    assign q = r;
endmodule
