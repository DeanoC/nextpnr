// Mixed-signedness DSP configuration probes (signed X, unsigned Y with headroom).
module top #(parameter integer W = 9) (
    input wire FPGA_CLK1_50
);
    wire [31:0] fpga_to_hps;
    wire [31:0] hps_to_fpga;
    cyclonev_hps_interface_mpu_general_purpose hps_gp (.gp_in(fpga_to_hps), .gp_out(hps_to_fpga));
    reg signed [W-1:0] a = 0;
    reg [W-2:0] b = 0;
    always @(posedge FPGA_CLK1_50) begin
        a <= {a[W-2:0], hps_to_fpga[0]};
        b <= {b[W-3:0], hps_to_fpga[1]};
    end
    wire signed [2*W-1:0] y0 = a * $signed({1'b0, b});
    reg [15:0] out = 0;
    integer i;
    always @(posedge FPGA_CLK1_50) begin
        out <= 0;
        for (i = 0; i < 2*W; i = i + 1) out[i % 16] <= out[i % 16] ^ y0[i];
    end
    assign fpga_to_hps = {16'hD61C, out};
endmodule
