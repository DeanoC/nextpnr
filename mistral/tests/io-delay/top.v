module top(input clk, input din, output dout);
    reg sample = 0;
    always @(posedge clk)
        sample <= din;
    assign dout = sample;
endmodule
