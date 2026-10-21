module top(input clk, input din, output dout);
(* preserve *) reg q0;
always @(posedge clk) q0 <= din;
wire n1;
cyclonev_lcell_comb #(.lut_mask(64'h5555555555555555), .shared_arith("off"), .extended_lut("off")) l1 (.dataa(q0), .datab(1'b1), .datac(1'b1), .datad(1'b1), .datae(1'b1), .dataf(1'b1), .datag(1'b1), .cin(1'b0), .sharein(1'b0), .combout(n1));
wire n2;
cyclonev_lcell_comb #(.lut_mask(64'h5555555555555555), .shared_arith("off"), .extended_lut("off")) l2 (.dataa(n1), .datab(1'b1), .datac(1'b1), .datad(1'b1), .datae(1'b1), .dataf(1'b1), .datag(1'b1), .cin(1'b0), .sharein(1'b0), .combout(n2));
wire n3;
cyclonev_lcell_comb #(.lut_mask(64'h5555555555555555), .shared_arith("off"), .extended_lut("off")) l3 (.dataa(n2), .datab(1'b1), .datac(1'b1), .datad(1'b1), .datae(1'b1), .dataf(1'b1), .datag(1'b1), .cin(1'b0), .sharein(1'b0), .combout(n3));
(* preserve *) reg qc;
always @(posedge clk) qc <= n3;
assign dout = qc;
endmodule
