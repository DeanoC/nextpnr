// HPS GP diagnostic: write clock 50 MHz, independently gated read clock 25 MHz.
module top #(parameter WIDTH = 20)(input FPGA_CLK1_50);
    localparam ABITS = WIDTH == 40 ? 8 : 9;
    wire [31:0] gp_in, gp_out;
    wire pll_clock, read_clock, locked;
    altera_pll #(.reference_clock_frequency("50.0 MHz"), .number_of_clocks(1),
        .output_clock_frequency0("25.0 MHz"), .phase_shift0("0 ps"),
        .duty_cycle0(50), .operation_mode("direct"), .fractional_vco_multiplier("false"))
        pll(.refclk(FPGA_CLK1_50), .rst(1'b0), .outclk(pll_clock), .locked(locked));
    cyclonev_clkena #(.clock_type("global clock"), .ena_register_mode("falling edge"),
        .ena_register_power_up("high"), .disable_mode("low"), .test_syn("high"))
        gate(.inclk(pll_clock), .ena(gp_out[29]), .outclk(read_clock), .enaout());
    (* ramstyle = "M10K" *) reg [WIDTH-1:0] mem [0:(1<<ABITS)-1];
    reg [WIDTH-1:0] q;
    function [19:0] initial_word;
        input integer address;
        initial_word = (address * 73) ^ (address >> 1) ^ 20'ha6;
    endfunction
    integer i;
    initial for (i = 0; i < (1<<ABITS); i = i + 1)
        if (WIDTH == 40) mem[i] = {~initial_word(i), initial_word(i)};
        else mem[i] = initial_word(i);
    reg [ABITS-1:0] waddr = 0;
    reg [19:0] wdata = 0;
    reg we = 0;
    // kit.py's post-program SPI probe also drives GP_OUT. Require an explicit
    // command before accepting writes so it cannot corrupt initialized RAM.
    reg armed = 0;
    always @(posedge FPGA_CLK1_50) begin
        if (gp_out == 32'h13579bdf) armed <= 1;
        waddr <= gp_out[ABITS-1:0];
        wdata <= gp_out[28:9];
        we <= armed && gp_out[31];
        if (we) mem[waddr] <= WIDTH == 40 ? {~wdata, wdata} : wdata;
    end
    always @(posedge read_clock)
        if (gp_out[30]) q <= mem[gp_out[ABITS-1:0]];
    cyclonev_hps_interface_mpu_general_purpose hps_gp(.gp_in(gp_in), .gp_out(gp_out));
    reg [15:0] sampled = 0;
    always @(posedge FPGA_CLK1_50) sampled <= q >> {gp_out[28:27], 4'b0};
    assign gp_in = {16'hD411, sampled};
endmodule
