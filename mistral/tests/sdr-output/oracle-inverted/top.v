module top(input FPGA_CLK1_50, input DATA, input CLEAR, output SDR_OUT);
reg q=0; always @(posedge FPGA_CLK1_50) if(CLEAR) q<=0; else q<=DATA; assign SDR_OUT=~q; endmodule
