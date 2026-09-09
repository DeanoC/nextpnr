module top(input FPGA_CLK1_50, input DATA, output OBSERVED_H, output OBSERVED_L);
    wire data_h, data_l;

    altddio_in #(.width(1), .intended_device_family("Cyclone V"),
      .power_up_high("OFF"), .invert_input_clocks("OFF"))
    ddr(.datain(DATA), .inclock(FPGA_CLK1_50), .inclocken(1'b1),
      .aset(1'b0), .aclr(1'b0), .sset(1'b0), .sclr(1'b0),
      .dataout_h(data_h), .dataout_l(data_l));

    assign OBSERVED_H = data_h;
    assign OBSERVED_L = data_l;
endmodule
