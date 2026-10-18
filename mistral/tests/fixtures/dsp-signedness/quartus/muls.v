(* multstyle = "dsp" *) module mul_u9_q(input [8:0] a, input [8:0] b, output [17:0] y); assign y = a * b; endmodule
(* multstyle = "dsp" *) module mul_s9_q(input signed [8:0] a, input signed [8:0] b, output signed [17:0] y); assign y = a * b; endmodule
(* multstyle = "dsp" *) module mul_u18_q(input [17:0] a, input [17:0] b, output [35:0] y); assign y = a * b; endmodule
(* multstyle = "dsp" *) module mul_s18_q(input signed [17:0] a, input signed [17:0] b, output signed [35:0] y); assign y = a * b; endmodule
(* multstyle = "dsp" *) module mul_u27_q(input [26:0] a, input [26:0] b, output [53:0] y); assign y = a * b; endmodule
(* multstyle = "dsp" *) module mul_s27_q(input signed [26:0] a, input signed [26:0] b, output signed [53:0] y); assign y = a * b; endmodule
