// cyclonev_mac atoms with explicit operand signedness. Quartus moves the signed
// operand of an inferred mixed product onto X, so signed-Y-only M9X9/M27X27
// configurations exist only at atom level. atom_xs9_q, atom_ss9_q and
// atom_xs27_q are controls that reproduce mul_m9_q, mul_s9_q and mul_mx27_q.
module atom_xs9_q(input signed [8:0] a, input [8:0] b, output [17:0] y);
  cyclonev_mac #(.operation_mode("m9x9"), .ax_width(9), .ay_scan_in_width(9), .result_a_width(18),
                 .signed_max("true"), .signed_may("false"), .signed_mbx("true"), .signed_mby("false"))
    mac (.ax(a), .ay(b), .aclr(2'b00), .resulta(y));
endmodule
module atom_ss9_q(input signed [8:0] a, input signed [8:0] b, output [17:0] y);
  cyclonev_mac #(.operation_mode("m9x9"), .ax_width(9), .ay_scan_in_width(9), .result_a_width(18),
                 .signed_max("true"), .signed_may("true"), .signed_mbx("true"), .signed_mby("true"))
    mac (.ax(a), .ay(b), .aclr(2'b00), .resulta(y));
endmodule
module atom_xs27_q(input signed [26:0] a, input [25:0] b, output [53:0] y);
  cyclonev_mac #(.operation_mode("m27x27"), .ax_width(27), .ay_scan_in_width(27), .result_a_width(54),
                 .signed_max("true"), .signed_may("false"), .signed_mbx("true"), .signed_mby("false"))
    mac (.ax(a), .ay({1'b0, b}), .aclr(2'b00), .resulta(y));
endmodule
module atom_ys9_q(input [8:0] a, input signed [8:0] b, output [17:0] y);
  cyclonev_mac #(.operation_mode("m9x9"), .ax_width(9), .ay_scan_in_width(9), .result_a_width(18),
                 .signed_max("false"), .signed_may("true"), .signed_mbx("false"), .signed_mby("true"))
    mac (.ax(a), .ay(b), .aclr(2'b00), .resulta(y));
endmodule
module atom_ys27_q(input [26:0] a, input signed [26:0] b, output [53:0] y);
  cyclonev_mac #(.operation_mode("m27x27"), .ax_width(27), .ay_scan_in_width(27), .result_a_width(54),
                 .signed_max("false"), .signed_may("true"), .signed_mbx("false"), .signed_mby("true"))
    mac (.ax(a), .ay(b), .aclr(2'b00), .resulta(y));
endmodule
