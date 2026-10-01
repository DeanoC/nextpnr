(* blackbox *)
module MISTRAL_ALUT2 #(parameter [3:0] LUT = 4'h0) (
    input wire A,
    input wire B,
    output wire Q
);
endmodule

(* blackbox *)
module MISTRAL_CONST #(parameter [0:0] LUT = 1'b0) (
    output wire Q
);
endmodule

(* blackbox *)
module MISTRAL_FF (
    input wire DATAIN,
    input wire CLK,
    input wire ACLR,
    input wire ENA,
    input wire SCLR,
    input wire SLOAD,
    input wire SDATA,
    output wire Q
);
endmodule

(* blackbox *)
module MISTRAL_IB (
    input wire PAD,
    output wire O
);
endmodule

(* blackbox *)
module MISTRAL_OB (
    input wire I,
    output wire PAD
);
endmodule

// Seal right-PCM bits 6, 7, and 8: a bottom 2-input LUT in a LAB that also
// holds a BEL-locked socket flip-flop. Placement leaves that LUT on
// comb_pinmap (E0/F0). The bottom half reads D/E1/F1.
//
// Seed 6 constant 512: locked plug_rdata_ff_10 with no LUT in its ALM.
// Skipping route-through leaves the constant on fabric F1.
module top (
    input wire clk,
    input wire pcm_a,
    input wire pcm_b,
    output wire pcm_q,
    output wire rdata_q
);
    wire zero;

    (* BEL = "MISTRAL_COMB.7.11.1" *)
    MISTRAL_ALUT2 #(.LUT(4'b1000)) seal_pcm_bits_678 (
        .A(pcm_a),
        .B(pcm_b),
        .Q(pcm_q)
    );

    (* BEL = "MISTRAL_COMB.7.11.13" *)
    MISTRAL_CONST #(.LUT(1'b0)) const0 (.Q(zero));

    (* BEL = "MISTRAL_FF.7.11.10" *)
    MISTRAL_FF plug_rdata_ff_10 (
        .DATAIN(zero),
        .CLK(clk),
        .ENA(1'b1),
        .Q(rdata_q)
    );
endmodule
