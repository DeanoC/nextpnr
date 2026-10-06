module top(input FPGA_CLK1_50, input DATA, input CLEAR, input ENABLE, output SDR_OUT);
    reg q = 0;
    always @(posedge FPGA_CLK1_50)
        if (ENABLE) begin
            if (CLEAR) q <= 0;
            else q <= DATA;
        end
    assign SDR_OUT = ~q;
endmodule
