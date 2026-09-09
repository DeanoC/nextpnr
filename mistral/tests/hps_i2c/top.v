module top (
    inout  wire HDMI_I2C_SCL,
    inout  wire HDMI_I2C_SDA
);
    wire scl_in;
    wire sda_in;
    wire scl_low;
    wire sda_low;

    // Drive zero only while the HPS asserts its low-enable.  The released
    // state is represented by OE=0, so neither pad can ever drive high.
    MISTRAL_IO scl_pad (
        .PAD(HDMI_I2C_SCL),
        .I(1'b0),
        .OE(scl_low),
        .O(scl_in)
    );
    MISTRAL_IO sda_pad (
        .PAD(HDMI_I2C_SDA),
        .I(1'b0),
        .OE(sda_low),
        .O(sda_in)
    );

    (* BEL = "cyclonev_hps_interface_peripheral_i2c.52.60.0" *)
    cyclonev_hps_interface_peripheral_i2c hdmi_i2c (
        .scl(scl_in),
        .sda(sda_in),
        .out_clk(scl_low),
        .out_data(sda_low)
    );
endmodule
