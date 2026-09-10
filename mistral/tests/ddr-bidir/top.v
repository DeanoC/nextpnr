module top(input wire FPGA_CLK1_50, inout wire DDR_IO);
    wire [31:0] gpo;
    wire [31:0] gpi;
    wire data_h = gpo[0];
    wire data_l = gpo[1];
    wire oe = gpo[2];
    wire dataout_h;
    wire dataout_l;
    wire combout;

    cyclonev_hps_interface_mpu_general_purpose hps_gp (.gp_in(gpi), .gp_out(gpo));
    assign gpi = {16'hDB01, 10'b0, oe, combout, dataout_h, dataout_l, data_h, data_l};

    altddio_bidir #(
        .width(1),
        .intended_device_family("Cyclone V"),
        .power_up_high("OFF"),
        .oe_reg("UNREGISTERED"),
        .extend_oe_disable("OFF"),
        .implement_input_in_lcell("UNUSED"),
        .invert_output("OFF")
    ) ddr (
        .datain_h(data_h),
        .datain_l(data_l),
        .inclock(FPGA_CLK1_50),
        .inclocken(1'b1),
        .outclock(FPGA_CLK1_50),
        .outclocken(1'b1),
        .aset(1'b0),
        .aclr(1'b0),
        .sset(1'b0),
        .sclr(1'b0),
        .oe(oe),
        .dataout_h(dataout_h),
        .dataout_l(dataout_l),
        .combout(combout),
        .oe_out(),
        .dqsundelayedout(),
        .padio(DDR_IO)
    );
endmodule
