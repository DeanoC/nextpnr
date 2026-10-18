(* multstyle = "dsp" *) module mac_sx18_q(input signed [17:0] a, input [16:0] b, input signed [35:0] c, output signed [36:0] y); assign y = a * $signed({1'b0, b}) + c; endmodule
(* multstyle = "dsp" *) module mac_sy18_q(input [16:0] a, input signed [17:0] b, input signed [35:0] c, output signed [36:0] y); assign y = $signed({1'b0, a}) * b + c; endmodule
(* multstyle = "dsp" *) module mul_mx27_q(input signed [26:0] a, input [25:0] b, output signed [53:0] y); assign y = a * $signed({1'b0, b}); endmodule
(* multstyle = "dsp" *) module mul_my27_q(input [25:0] a, input signed [26:0] b, output signed [53:0] y); assign y = $signed({1'b0, a}) * b; endmodule
(* multstyle = "dsp" *) module mul_my9_q(input [7:0] a, input signed [8:0] b, output signed [17:0] y); assign y = $signed({1'b0, a}) * b; endmodule
