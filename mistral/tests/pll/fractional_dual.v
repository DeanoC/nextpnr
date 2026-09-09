module top(input wire FPGA_CLK1_50);
    wire [1:0] clocks;
    wire audio_clock = clocks[0];
    wire locked;
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
        .reference_clock_frequency("50.0 MHz"), .number_of_clocks(2),
        .output_clock_frequency0("12.288 MHz"), .output_clock_frequency1("24.576 MHz"),
        .phase_shift0("0 ps"), .phase_shift1("0 ps"),
        .duty_cycle0(50), .duty_cycle1(50), .operation_mode("direct"),
        .fractional_vco_multiplier("true")
    ) pll (.refclk(FPGA_CLK1_50), .rst(reset_sync), .outclk(clocks), .locked(locked));
    cyclonev_hps_interface_mpu_general_purpose hps_gp (.gp_in(gpi), .gp_out(gpo));
    pll_meter meter (
        .refclk(FPGA_CLK1_50), .testclk(audio_clock), .locked(locked), .request(gpo[1]),
        .busy(busy), .done(done), .lost_lock(lost_lock),
        .lock_status(lock_status), .result(result)
    );
    wire [15:0] result1;
    wire busy1, done1, lost1, lock1;
    pll_meter meter1 (
        .refclk(FPGA_CLK1_50), .testclk(clocks[1]), .locked(locked), .request(gpo[1]),
        .busy(busy1), .done(done1), .lost_lock(lost1),
        .lock_status(lock1), .result(result1)
    );
    // GPO[3] selects both the result and its matching meter status.
    wire [15:0] selected = gpo[3] ? result1 : result;
    wire selected_busy = gpo[3] ? busy1 : busy;
    wire selected_done = gpo[3] ? done1 : done;
    wire selected_lock = gpo[3] ? lock1 : lock_status;
    wire selected_lost = gpo[3] ? lost1 : lost_lock;
    assign gpi = {16'hD717, selected_busy, selected_done, selected_lock, selected_lost, reset_sync, 3'b0,
                  gpo[0] ? selected[15:8] : selected[7:0]};
endmodule
