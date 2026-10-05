module top(input FPGA_CLK1_50, input [1:0] KEY, input [3:0] SW, output [7:0] LED);
    reg [31:0] c;
    always @(posedge FPGA_CLK1_50) c <= c + 1'b1;
    assign LED = c[31:24] ^ {KEY, SW, 2'b00};
endmodule
