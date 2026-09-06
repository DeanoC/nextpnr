module top(input wire FPGA_CLK1_50);
    wire clk25;
    wire locked;
    wire [31:0] gpo;
    wire [31:0] gpi;
    wire busy, done, lost_lock, lock_status;
    wire [15:0] result;
    altera_pll #(
        .reference_clock_frequency("50.0 MHz"),
        .number_of_clocks(1),
        .output_clock_frequency0("25.0 MHz"),
        .phase_shift0("0 ps"),
        .duty_cycle0(50),
        .operation_mode("direct"),
        .fractional_vco_multiplier("false")
    ) pll (
        .refclk(FPGA_CLK1_50), .rst(1'b0),
        .outclk(clk25), .locked(locked)
    );
    cyclonev_hps_interface_mpu_general_purpose hps_gp (.gp_in(gpi), .gp_out(gpo));
    pll_meter meter (
        .refclk(FPGA_CLK1_50), .testclk(clk25), .locked(locked), .request(gpo[1]),
        .busy(busy), .done(done), .lost_lock(lost_lock),
        .lock_status(lock_status), .result(result)
    );
    // Snapshot remains unchanged until the next request, so byte reads are coherent.
    assign gpi = {16'hD711, busy, done, lock_status, lost_lock, 4'b0,
                  gpo[0] ? result[15:8] : result[7:0]};
endmodule
