// Explicit primitives keep the plain-D and shared-SCLR experiments distinct.
(* blackbox *) module MISTRAL_FF (
    input DATAIN, CLK, ENA, ACLR, SCLR, SLOAD, SDATA, output Q
); endmodule
(* blackbox *) module MISTRAL_ALUT2 #(parameter [3:0] LUT = 4'b0010) (
    input A, B, output Q
); endmodule
(* blackbox *) module MISTRAL_IB (input PAD, output O); endmodule
(* blackbox *) module MISTRAL_OB (input I, output PAD); endmodule

module top(input clk, clear_masks, lower_data, upper_data, output dqml, dqmh);
    wire lower_d, upper_d, sclr;
`ifdef MASK_PLAIN_D
    MISTRAL_ALUT2 lower_lut (.A(lower_data), .B(clear_masks), .Q(lower_d));
    MISTRAL_ALUT2 upper_lut (.A(upper_data), .B(clear_masks), .Q(upper_d));
    assign sclr = 1'b0;
`else
    assign lower_d = lower_data;
    assign upper_d = upper_data;
    assign sclr = clear_masks;
`endif
    // The original sealed shell's mask LAB and top/bottom register halves.
    (* BEL = "MISTRAL_FF.42.3.56" *)
    MISTRAL_FF lower_ff (.DATAIN(lower_d), .CLK(clk), .ENA(1'b1), .ACLR(1'b1),
        .SCLR(sclr), .SLOAD(1'b0), .SDATA(1'b0), .Q(dqml));
    (* BEL = "MISTRAL_FF.42.3.16" *)
    MISTRAL_FF upper_ff (.DATAIN(upper_d), .CLK(clk), .ENA(1'b1), .ACLR(1'b1),
        .SCLR(sclr), .SLOAD(1'b0), .SDATA(1'b0), .Q(dqmh));
endmodule
