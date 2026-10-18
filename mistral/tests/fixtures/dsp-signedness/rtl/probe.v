// Generic DSP configuration probe: operands shift in serially from HPS GPO so
// the design has no pin dependence; every product bit folds into GPI.
module top #(parameter integer W = 9, parameter integer SIGNED = 0, parameter integer LANES = 1) (
    input wire FPGA_CLK1_50
);
    wire [31:0] fpga_to_hps;
    wire [31:0] hps_to_fpga;
    cyclonev_hps_interface_mpu_general_purpose hps_gp (.gp_in(fpga_to_hps), .gp_out(hps_to_fpga));

    reg [W-1:0] a = 0, b = 0, c = 0;
    always @(posedge FPGA_CLK1_50) begin
        a <= {a[W-2:0], hps_to_fpga[0]};
        b <= {b[W-2:0], hps_to_fpga[1]};
        c <= {c[W-2:0], hps_to_fpga[2]};
    end

    wire [2*W-1:0] y0, y1, y2;
    generate if (SIGNED) begin : s
        assign y0 = $signed(a) * $signed(b);
        assign y1 = $signed(b) * $signed(c);
        assign y2 = $signed(a) * $signed(c);
    end else begin : u
        assign y0 = a * b;
        assign y1 = b * c;
        assign y2 = a * c;
    end endgenerate

    wire [2*W-1:0] folded = (LANES > 1) ? (y0 ^ y1 ^ y2) : y0;
    reg [15:0] out = 0;
    integer i;
    always @(posedge FPGA_CLK1_50) begin
        out <= 0;
        for (i = 0; i < 2*W; i = i + 1) out[i % 16] <= out[i % 16] ^ folded[i];
    end
    assign fpga_to_hps = {16'hD61B, out};
endmodule
