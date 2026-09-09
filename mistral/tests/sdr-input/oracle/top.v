module top(input FPGA_CLK1_50, input DATA, output OBSERVED);
    reg captured;

    always @(posedge FPGA_CLK1_50)
        captured <= DATA;

    assign OBSERVED = captured;
endmodule
