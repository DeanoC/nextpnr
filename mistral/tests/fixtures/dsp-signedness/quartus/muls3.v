(* multstyle = "dsp" *) module mul3_s9_q(input signed [8:0] a, input signed [8:0] b, input signed [8:0] c, output signed [17:0] y0, output signed [17:0] y1, output signed [17:0] y2); assign y0 = a * b; assign y1 = b * c; assign y2 = a * c; endmodule
(* multstyle = "dsp" *) module mul3_u9_q(input [8:0] a, input [8:0] b, input [8:0] c, output [17:0] y0, output [17:0] y1, output [17:0] y2); assign y0 = a * b; assign y1 = b * c; assign y2 = a * c; endmodule
(* multstyle = "dsp" *) module mul_m18_q(input signed [17:0] a, input [17:0] b, output signed [36:0] y); assign y = a * $signed({1'b0, b}); endmodule
(* multstyle = "dsp" *) module mul_m9_q(input signed [8:0] a, input [8:0] b, output signed [18:0] y); assign y = a * $signed({1'b0, b}); endmodule
