// Direct equal-width true-dual-port M10K fixture.
//
// The locked Yosys M10K TDP library currently emits only the 10- and 20-bit
// rules.  Keep this fixture primitive-based so nextpnr can exercise the
// Cyclone V's native 8192x1, 4096x2 and 2048x5 geometries independently of
// that inference limitation.
(* blackbox *) module MISTRAL_M10K_TDP #(
    parameter CFG_ABITS = 10,
    parameter CFG_DBITS = 10,
    parameter CFG_RD_ABITS = CFG_ABITS,
    parameter CFG_RD_DBITS = CFG_DBITS,
    parameter CFG_MIXED_WIDTH = 0,
    parameter CFG_BYTE_ENABLE = 0
) (
    input CLK1,
    input CLK2,
    input [CFG_ABITS-1:0] A1ADDR,
    input [CFG_RD_ABITS-1:0] B1ADDR,
    input [CFG_DBITS-1:0] A1DATA,
    input [CFG_RD_DBITS-1:0] B1DATA,
    output [CFG_DBITS-1:0] A1Q,
    output [CFG_RD_DBITS-1:0] B1Q,
    input A1EN,
    input B1EN,
    input A1WE,
    input B1WE,
    input [1:0] A1BE,
    input [1:0] B1BE,
    input ACLR0,
    input ACLR1
);
endmodule

module top #(
    parameter WIDTH = 1,
    parameter ABITS = 13
) (
    input FPGA_CLK1_50,
    output LED
);
    wire [WIDTH-1:0] qa, qb;
    reg sample;

    MISTRAL_M10K_TDP #(
        .CFG_ABITS(ABITS),
        .CFG_DBITS(WIDTH)
    ) ram (
        .CLK1(FPGA_CLK1_50),
        .CLK2(FPGA_CLK1_50),
        .A1ADDR({ABITS{1'b0}}),
        .B1ADDR({ABITS{1'b0}}),
        .A1DATA({WIDTH{1'b0}}),
        .B1DATA({WIDTH{1'b0}}),
        .A1Q(qa),
        .B1Q(qb),
        .A1EN(1'b1),
        .B1EN(1'b1),
        .A1WE(1'b0),
        .B1WE(1'b0),
        .ACLR0(1'b0),
        .ACLR1(1'b0)
    );

    always @(posedge FPGA_CLK1_50)
        sample <= qa[0] ^ qb[0];

    assign LED = sample;
endmodule
