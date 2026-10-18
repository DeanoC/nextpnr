// Mixed-signedness DSP configuration probe: a signed W-bit operand times an
// unsigned U-bit operand on one MISTRAL_MUL9X9 (W = 9) or MISTRAL_MUL27X27
// (W = 27). SIGNED_A = 1 puts the signed operand on A (X), 0 on B (Y).
//
// The cell is instantiated because Yosys does not infer the signed-X order from
// the Quartus reference RTL: with a narrower unsigned operand,
// a * $signed({1'b0, b}) stays signed on both operands (see the README).
module top #(parameter integer W = 9, parameter integer U = W - 1, parameter integer SIGNED_A = 1) (
    input wire FPGA_CLK1_50
);
    wire [31:0] fpga_to_hps;
    wire [31:0] hps_to_fpga;
    cyclonev_hps_interface_mpu_general_purpose hps_gp (.gp_in(fpga_to_hps), .gp_out(hps_to_fpga));

    reg [W-1:0] s = 0;
    reg [U-1:0] u = 0;
    always @(posedge FPGA_CLK1_50) begin
        s <= {s[W-2:0], hps_to_fpga[0]};
        u <= {u[U-2:0], hps_to_fpga[1]};
    end

    wire [W-1:0] u_ext = u;
    wire [W-1:0] a = SIGNED_A ? s : u_ext;
    wire [W-1:0] b = SIGNED_A ? u_ext : s;
    wire [2*W-1:0] y0;
    generate if (W == 9) begin : m9
        MISTRAL_MUL9X9 #(.A_SIGNED(SIGNED_A), .B_SIGNED(1 - SIGNED_A)) mul (.A(a), .B(b), .Y(y0));
    end else if (W == 27) begin : m27
        MISTRAL_MUL27X27 #(.A_SIGNED(SIGNED_A), .B_SIGNED(1 - SIGNED_A)) mul (.A(a), .B(b), .Y(y0));
    end endgenerate

    reg [15:0] out = 0;
    integer i;
    always @(posedge FPGA_CLK1_50) begin
        out <= 0;
        for (i = 0; i < 2*W; i = i + 1) out[i % 16] <= out[i % 16] ^ y0[i];
    end
    assign fpga_to_hps = {16'hD61C, out};
endmodule
