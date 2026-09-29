module top(input clk, input rst, input d, output q);
    reg q_rst;
    reg q_free;
    always @(posedge clk or posedge rst) begin
        if (rst)
            q_rst <= 1'b0;
        else
            q_rst <= d;
    end
    always @(posedge clk)
        q_free <= d;
    assign q = q_rst ^ q_free;
endmodule
