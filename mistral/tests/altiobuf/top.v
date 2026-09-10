module top(
    input  wire ALTI_IN,
    output wire ALTI_OUT,
    inout  wire ALTI_BIDIR,
    input  wire ALTI_OE,
    output wire BIDIR_DATA
);
    wire in_data;
    altiobuf_in #(
        .number_of_channels(1),
        .enable_bus_hold("FALSE"),
        .use_differential_mode("FALSE")
    ) in_buf (
        .datain(ALTI_IN),
        .dataout(in_data)
    );

    altiobuf_out #(
        .number_of_channels(1),
        .enable_bus_hold("FALSE"),
        .use_differential_mode("FALSE"),
        .use_oe("FALSE")
    ) out_buf (
        .datain(in_data),
        .dataout(ALTI_OUT)
    );

    altiobuf_bidir #(
        .number_of_channels(1),
        .enable_bus_hold("OFF")
    ) bidir_buf (
        .dataio(ALTI_BIDIR),
        .oe(ALTI_OE),
        .datain(in_data),
        .dataout(BIDIR_DATA)
    );
endmodule
