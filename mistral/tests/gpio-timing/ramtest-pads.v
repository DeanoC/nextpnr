// Reference pad coverage on the FES/DE10-Nano SDRAM pin map.
// Vary both DDR output data words to exercise both pin transitions at each
// clock phase. SDRAM_CLK is a timing fixture output, not a usable SDRAM clock.
module top(input FPGA_CLK1_50,
    inout [15:0] SDRAM_DQ, output reg [12:0] SDRAM_A,
    output reg [1:0] SDRAM_BA, output reg SDRAM_CKE,
    output reg SDRAM_nWE, SDRAM_nCAS, SDRAM_nCS, SDRAM_nRAS,
    output reg SDRAM_DQML, SDRAM_DQMH, output SDRAM_CLK,
    output [31:0] q);
    reg [15:0] beat = 0;
    reg [15:0] data_out;
    reg data_oe;
    always @(posedge FPGA_CLK1_50) begin
        beat <= beat + 1'b1;
        data_out <= beat;
        data_oe <= beat[8];
        SDRAM_A <= beat[12:0];
        SDRAM_BA <= beat[14:13];
        {SDRAM_CKE, SDRAM_nWE, SDRAM_nCAS, SDRAM_nCS, SDRAM_nRAS} <= beat[4:0];
        {SDRAM_DQML, SDRAM_DQMH} <= beat[6:5];
    end
    genvar i;
    generate for (i = 0; i < 16; i = i + 1) begin : pads
        wire pin;
        altiobuf_bidir #(.number_of_channels(1), .enable_bus_hold("FALSE")) pad(
            .dataio(SDRAM_DQ[i]), .oe(data_oe), .datain(data_out[i]), .dataout(pin));
        altddio_in #(.width(1), .intended_device_family("Cyclone V"),
            .power_up_high("OFF"), .invert_input_clocks("OFF")) cap(
            .datain(pin), .inclock(FPGA_CLK1_50), .inclocken(1'b1),
            .aset(1'b0), .aclr(1'b0), .sset(1'b0), .sclr(1'b0),
            .dataout_h(q[2*i+1]), .dataout_l(q[2*i]));
    end endgenerate
    altddio_out #(.width(1), .intended_device_family("Cyclone V"),
        .power_up_high("OFF"), .oe_reg("UNREGISTERED"),
        .extend_oe_disable("OFF"), .invert_output("OFF")) clk_pad(
        .datain_h(beat[0]), .datain_l(beat[1]), .outclock(FPGA_CLK1_50),
        .outclocken(1'b1), .aset(1'b0), .aclr(1'b0), .sset(1'b0), .sclr(1'b0),
        .oe(1'b1), .dataout(SDRAM_CLK), .oe_out());
endmodule
