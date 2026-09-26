module top(input FPGA_CLK1_50, output DDR_OUT);
    reg [1:0] payload = 2'b00;

    always @(posedge FPGA_CLK1_50)
        payload <= payload + 2'b01;

    altddio_out #(.width(1), .intended_device_family("Cyclone V"),
      .power_up_high("OFF"), .oe_reg("UNREGISTERED"),
      .extend_oe_disable("OFF"), .invert_output("OFF"))
    ddr(.datain_h(payload[0]), .datain_l(payload[1]),
      .outclock(FPGA_CLK1_50), .outclocken(1'b1),
      .aset(1'b0), .aclr(1'b0), .sset(1'b0), .sclr(1'b0),
      .oe(1'b1), .dataout(DDR_OUT), .oe_out());
endmodule
