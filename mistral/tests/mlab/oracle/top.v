module top(input clk, input we, input [4:0] wa, ra, input [19:0] d, output [19:0] q);
(* ramstyle = "MLAB, no_rw_check" *) reg [19:0] mem[0:31];
initial begin
mem[0] = 20'ha65b9;
mem[1] = 20'hbc9ab;
mem[2] = 20'h9ccdb;
mem[3] = 20'hf8389;
mem[4] = 20'hef265;
mem[5] = 20'he02ff;
mem[6] = 20'h6d2af;
mem[7] = 20'h7400d;
mem[8] = 20'hc5e61;
mem[9] = 20'hb0b03;
mem[10] = 20'ha8c23;
mem[11] = 20'h9c091;
mem[12] = 20'hee64d;
mem[13] = 20'h37467;
mem[14] = 20'h1b1b7;
mem[15] = 20'hfda45;
mem[16] = 20'ha4389;
mem[17] = 20'he3b9b;
mem[18] = 20'h22f6b;
mem[19] = 20'he41f9;
mem[20] = 20'hb2ad5;
mem[21] = 20'h037cf;
mem[22] = 20'h7533f;
mem[23] = 20'hbf39d;
mem[24] = 20'h4d531;
mem[25] = 20'hd1e53;
mem[26] = 20'h7b633;
mem[27] = 20'h061a1;
mem[28] = 20'hbb03d;
mem[29] = 20'h2c217;
mem[30] = 20'ha15c7;
mem[31] = 20'h2f875;
end
always @(posedge clk) if (we) mem[wa] <= d;
assign q = mem[ra];
endmodule
