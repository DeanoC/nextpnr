module top(input FPGA_CLK1_50, input DATA, output reg SDR_OUT);
    always @(posedge FPGA_CLK1_50)
        SDR_OUT <= DATA;
endmodule
