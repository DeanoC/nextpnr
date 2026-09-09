(* blackbox *)
module MISTRAL_ALUT2 #(parameter [3:0] LUT = 4'h0) (
    input wire A, B,
    output wire Q
);
endmodule

(* blackbox *)
module MISTRAL_FF (
    input wire DATAIN, CLK, ACLR, ENA, SCLR, SLOAD, SDATA,
    output wire Q
);
endmodule

(* blackbox *)
module MISTRAL_IB (input wire PAD, output wire O);
endmodule

(* blackbox *)
module MISTRAL_OB (input wire I, output wire PAD);
endmodule

module top (
    input  wire clk,
    input  wire signal,
    output wire q_zero,
    output wire q_one,
    output wire q_signal,
    output wire [2:0] active
);
    (* BEL = "MISTRAL_COMB.7.11.37" *)
    MISTRAL_ALUT2 #(.LUT(4'b0110)) active_zero (
        .A(signal), .B(q_signal), .Q(active[0])
    );
    (* BEL = "MISTRAL_COMB.7.11.43" *)
    MISTRAL_ALUT2 #(.LUT(4'b1001)) active_one (
        .A(signal), .B(q_signal), .Q(active[1])
    );
    (* BEL = "MISTRAL_COMB.7.11.49" *)
    MISTRAL_ALUT2 #(.LUT(4'b0110)) active_signal (
        .A(signal), .B(q_one), .Q(active[2])
    );
    (* BEL = "MISTRAL_FF.7.11.40" *)
    MISTRAL_FF ff_zero (
        .DATAIN(1'b0), .CLK(clk), .ACLR(1'b1), .ENA(1'b1),
        .SCLR(1'b0), .SLOAD(1'b0), .SDATA(1'b0), .Q(q_zero)
    );
    (* BEL = "MISTRAL_FF.7.11.46" *)
    MISTRAL_FF ff_one (
        .DATAIN(1'b1), .CLK(clk), .ACLR(1'b1), .ENA(1'b1),
        .SCLR(1'b0), .SLOAD(1'b0), .SDATA(1'b0), .Q(q_one)
    );
    (* BEL = "MISTRAL_FF.7.11.52" *)
    MISTRAL_FF ff_signal (
        .DATAIN(signal), .CLK(clk), .ACLR(1'b1), .ENA(1'b1),
        .SCLR(1'b0), .SLOAD(1'b0), .SDATA(1'b0), .Q(q_signal)
    );
endmodule
