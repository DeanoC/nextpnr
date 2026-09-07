module top(input wire FPGA_CLK1_50);
    wire video_clock, audio_clock;
    wire [1:0] locked;
    wire [31:0] gpo;
    reg [7:0] video_count = 0, audio_count = 0;
    always @(posedge video_clock) video_count <= video_count + 1'b1;
    always @(posedge audio_clock) audio_count <= audio_count + 1'b1;
    altera_pll #(
        .reference_clock_frequency("50.0 MHz"), .number_of_clocks(1),
        .output_clock_frequency0("12.288 MHz"),
        .phase_shift0("0 ps"), .duty_cycle0(50),
        .operation_mode("direct"), .fractional_vco_multiplier("true")
    ) video_pll (.refclk(FPGA_CLK1_50), .rst(1'b0), .outclk(video_clock), .locked(locked[0]));
    altera_pll #(
        .reference_clock_frequency("50.0 MHz"), .number_of_clocks(1),
        .output_clock_frequency0("25.0 MHz"),
        .phase_shift0("0 ps"), .duty_cycle0(50),
        .operation_mode("direct"), .fractional_vco_multiplier("false")
    ) audio_pll (.refclk(FPGA_CLK1_50), .rst(1'b0), .outclk(audio_clock), .locked(locked[1]));
    cyclonev_hps_interface_mpu_general_purpose hps_gp (
        .gp_in({14'b0, locked, audio_count, video_count}), .gp_out(gpo));
endmodule
