module top(input clk, input [5:0] din, output reg [1:0] dout);
(* preserve *) reg [5:0] q;
always @(posedge clk) q <= din;
wire y5,y6;
cyclonev_lcell_comb #(.lut_mask(64'h9669699696696996), .shared_arith("off"), .extended_lut("off")) lut5 (.dataa(q[0]),.datab(q[1]),.datac(q[2]),.datad(q[3]),.datae(q[4]),.dataf(1'b0),.datag(1'b0),.cin(1'b0),.sharein(1'b0),.combout(y5));
cyclonev_lcell_comb #(.lut_mask(64'hd2c4b6e1987a35f0), .shared_arith("off"), .extended_lut("off")) lut6 (.dataa(q[0]),.datab(q[1]),.datac(q[2]),.datad(q[3]),.datae(q[4]),.dataf(q[5]),.datag(1'b0),.cin(1'b0),.sharein(1'b0),.combout(y6));
always @(posedge clk) dout <= {y6,y5};
endmodule
