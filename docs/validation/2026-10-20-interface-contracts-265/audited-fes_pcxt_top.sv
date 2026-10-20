// SPDX-License-Identifier: GPL-2.0-or-later
// Autonomous diagnostic board shell. No production runtime/firmware ABI.
module top #(parameter bit MDA = 0, SCANLINES = 0) (
    input wire FPGA_CLK1_50,
    output wire HDMI_TX_CLK, HDMI_TX_DE, HDMI_TX_HS, HDMI_TX_VS,
    output wire [23:0] HDMI_TX_D,
    inout wire HDMI_I2C_SCL, HDMI_I2C_SDA,
    output wire SDRAM_CLK, SDRAM_CKE, SDRAM_nCS, SDRAM_nRAS, SDRAM_nCAS, SDRAM_nWE,
    output wire SDRAM_DQML, SDRAM_DQMH,
    output wire [1:0] SDRAM_BA,
    output wire [12:0] SDRAM_A,
    inout wire [15:0] SDRAM_DQ
);
    wire clk, clk_capture, clk_rom, clk_pixel, memory_locked, video_locked;
    // Rate-3 consumes the falling-edge sample. A 180 degree fabric capture
    // clock provides that edge without unsupported DDIO on bidirectional IO.
    wire [2:0] clocks;
    altera_pll #(
        .reference_clock_frequency("50.0 MHz"), .number_of_clocks(3),
        .output_clock_frequency0("100.0 MHz"), .output_clock_frequency1("100.0 MHz"),
        .output_clock_frequency2("100.0 MHz"),
        .phase_shift0("0 ps"), .phase_shift1("5000 ps"), .phase_shift2("2500 ps"),
        .duty_cycle0(50), .duty_cycle1(50), .duty_cycle2(50), .operation_mode("direct"),
        .fractional_vco_multiplier("false")
    ) memory_clock (.refclk(FPGA_CLK1_50), .rst(1'b0), .outclk(clocks), .locked(memory_locked));
    assign clk = clocks[0];
    assign clk_capture = clocks[1];
    assign clk_rom = clocks[2];
    altera_pll #(
        .reference_clock_frequency("50.0 MHz"), .number_of_clocks(1),
        .output_clock_frequency0("74.25 MHz"), .phase_shift0("0 ps"), .duty_cycle0(50),
        .operation_mode("direct"), .fractional_vco_multiplier("true")
    ) video_clock (.refclk(FPGA_CLK1_50), .rst(1'b0), .outclk(clk_pixel), .locked(video_locked));
    wire locked = memory_locked && video_locked;
    (* async_reg = "true" *) reg [1:0] sys_reset = 2'b11, pixel_reset = 2'b11;
    always @(posedge clk or negedge locked)
        if (!locked) sys_reset <= 2'b11;
        else sys_reset <= {sys_reset[0], 1'b0};
    always @(posedge clk_pixel or negedge locked)
        if (!locked) pixel_reset <= 2'b11;
        else pixel_reset <= {pixel_reset[0], 1'b0};
    wire firmware_valid, firmware_ready, firmware_done;
    wire [15:0] firmware_address;
    wire [7:0] firmware_data;
    pcxt_boot boot (.clk(clk), .cold_reset(sys_reset[1]), .enable(1'b1),
        .valid(firmware_valid), .ready(firmware_ready), .done(firmware_done),
        .address(firmware_address), .data(firmware_data));
    wire [31:0] video_request;
    wire [27:0] video_response;
    wire [15:0] dq_out, dq_sample;
    wire dq_oe;
    /* verilator lint_off PINCONNECTEMPTY */
    pcxt_system #(.MDA(MDA), .SCANLINES(SCANLINES)) system (
        .clk(clk), .clk_rom(clk_rom), .clk_pixel(clk_pixel), .clk_pin(clk), .cold_reset(sys_reset[1]),
        .reset_pixel(pixel_reset[1]), .warm_reset(1'b0), .hold(1'b0),
        .firmware_valid(firmware_valid), .firmware_ready(firmware_ready),
        .firmware_done(firmware_done), .firmware_address(firmware_address), .firmware_data(firmware_data),
        .firmware_loaded(), .initialized(), .machine_reset(), .fault(), .halted(),
        .dma_owner(), .refresh_done(), .audio_valid(), .audio_sample(),
        .video_request(video_request), .video_response(video_response),
        .sdram_clk(SDRAM_CLK), .sdram_cke(SDRAM_CKE), .sdram_ncs(SDRAM_nCS),
        .sdram_nras(SDRAM_nRAS), .sdram_ncas(SDRAM_nCAS), .sdram_nwe(SDRAM_nWE),
        .sdram_ba(SDRAM_BA), .sdram_a(SDRAM_A), .sdram_dqml(SDRAM_DQML), .sdram_dqmh(SDRAM_DQMH),
        .dq_out(dq_out), .dq_oe(dq_oe), .dq_rise(dq_sample), .dq_fall(dq_sample)
    );
    /* verilator lint_on PINCONNECTEMPTY */
    generate for (genvar n=0; n<16; n=n+1) begin: sdram_pads
        wire pin_value;
        altiobuf_bidir #(.number_of_channels(1), .enable_bus_hold("OFF")) pad (
            .dataio(SDRAM_DQ[n]), .oe(dq_oe), .datain(dq_out[n]), .dataout(pin_value));
        reg captured;
        always @(posedge clk_capture) captured <= pin_value;
        assign dq_sample[n] = captured;
    end endgenerate
    wire scl_low, sda_low, scl_in, sda_in;
    MISTRAL_IO scl (.I(1'b0), .OE(scl_low), .O(scl_in), .PAD(HDMI_I2C_SCL));
    MISTRAL_IO sda (.I(1'b0), .OE(sda_low), .O(sda_in), .PAD(HDMI_I2C_SDA));
    (* BEL = "cyclonev_hps_interface_peripheral_i2c.52.60.0" *)
    cyclonev_hps_interface_peripheral_i2c hdmi_i2c (
        .scl(scl_in), .sda(sda_in), .out_clk(scl_low), .out_data(sda_low));
    assign HDMI_TX_CLK = clk_pixel;
    assign HDMI_TX_D = video_response[23:0];
    assign HDMI_TX_DE = video_response[24];
    assign HDMI_TX_HS = video_response[25];
    assign HDMI_TX_VS = video_response[26];
    wire unused_video = ^{video_request, video_response[27]};
endmodule
