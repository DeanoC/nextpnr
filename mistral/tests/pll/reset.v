module top(input wire FPGA_CLK1_50);
    wire clk25, locked;
    wire [31:0] gpo;
    wire [31:0] gpi;
    wire busy, done, lost_lock, lock_status;
    wire [15:0] result;
    // Linux holds reset until it observes this reference-domain echo.
    (* async_reg = "true" *) reg reset_meta = 0;
    reg reset_sync = 0;
    always @(posedge FPGA_CLK1_50) begin
        reset_meta <= gpo[2];
        reset_sync <= reset_meta;
    end
    altera_pll #(
        .reference_clock_frequency("50.0 MHz"), .number_of_clocks(1),
        .output_clock_frequency0("25.0 MHz"), .phase_shift0("0 ps"),
        .duty_cycle0(50), .operation_mode("direct"),
        .fractional_vco_multiplier("false")
    ) pll (.refclk(FPGA_CLK1_50), .rst(reset_sync), .outclk(clk25), .locked(locked));
    cyclonev_hps_interface_mpu_general_purpose hps_gp (.gp_in(gpi), .gp_out(gpo));
    pll_meter meter (
        .refclk(FPGA_CLK1_50), .testclk(clk25), .locked(locked), .request(gpo[1]),
        .busy(busy), .done(done), .lost_lock(lost_lock),
        .lock_status(lock_status), .result(result)
    );
    assign gpi = {16'hD712, busy, done, lock_status, lost_lock, reset_sync, 3'b0,
                  gpo[0] ? result[15:8] : result[7:0]};
endmodule
