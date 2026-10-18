(* multstyle = "dsp" *) module mac_s18_q(input signed [17:0] a, input signed [17:0] b, input signed [35:0] c, output signed [36:0] y); assign y = a * b + c; endmodule
(* multstyle = "dsp" *) module mac_u18_q(input [17:0] a, input [17:0] b, input [35:0] c, output [36:0] y); assign y = a * b + c; endmodule
(* multstyle = "dsp" *) module mul2_s18_q(input signed [17:0] a, input signed [17:0] b, input signed [17:0] c, input signed [17:0] d, output signed [35:0] y0, output signed [35:0] y1); assign y0 = a * b; assign y1 = c * d; endmodule
(* multstyle = "dsp" *) module mul2_u18_q(input [17:0] a, input [17:0] b, input [17:0] c, input [17:0] d, output [35:0] y0, output [35:0] y1); assign y0 = a * b; assign y1 = c * d; endmodule
