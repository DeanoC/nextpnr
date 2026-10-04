module top(input clk, rst0, rst1, input [3:0] d, output [3:0] q);
    reg [3:0] r;
    always @(posedge clk or posedge rst0) if (rst0) r[0] <= 1'b0; else r[0] <= d[0];
    always @(posedge clk or posedge rst1) if (rst1) r[1] <= 1'b0; else r[1] <= d[1];
    always @(posedge clk or posedge rst0) if (rst0) r[2] <= 1'b0; else r[2] <= d[2];
    always @(posedge clk or posedge rst1) if (rst1) r[3] <= 1'b0; else r[3] <= d[3];
    assign q = r;
endmodule
