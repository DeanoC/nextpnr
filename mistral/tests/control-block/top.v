// Every supported Cyclone V control block atom in one design. The same RTL is
// compiled by Quartus (oracle) and by Yosys + nextpnr.
module top(
    input FPGA_CLK1_50,
    input [1:0] KEY,
    input [3:0] SW,
    output [7:0] LED,
    input altera_reserved_tms,
    input altera_reserved_tck,
    input altera_reserved_tdi,
    output altera_reserved_tdo
);
    // Load (shiftnld low) once, then shift the chip ID, CRC syndrome and
    // operation register out bit by bit.
    reg [6:0] count = 7'd0;
    reg shiftnld = 1'b0;
    always @(posedge FPGA_CLK1_50) begin
        if (count != 7'd127)
            count <= count + 1'b1;
        shiftnld <= count != 7'd0;
    end

    wire id_bit, crc_bit, crc_error, crc_end, opreg_bit;
    cyclonev_chipidblock chipid(.clk(FPGA_CLK1_50), .shiftnld(shiftnld), .regout(id_bit));
    cyclonev_crcblock #(.oscillator_divider(2)) crc(.clk(FPGA_CLK1_50), .shiftnld(shiftnld), .crcerror(crc_error),
                                                    .regout(crc_bit), .endofedfullchip(crc_end));
    cyclonev_opregblock opreg(.clk(FPGA_CLK1_50), .shiftnld(shiftnld), .regout(opreg_bit));

    reg [63:0] chip_id = 64'd0;
    reg [31:0] syndrome = 32'd0;
    reg [31:0] operation = 32'd0;
    reg crc_error_q = 1'b0, crc_end_q = 1'b0;
    always @(posedge FPGA_CLK1_50) begin
        chip_id <= {id_bit, chip_id[63:1]};
        syndrome <= {crc_bit, syndrome[31:1]};
        operation <= {opreg_bit, operation[31:1]};
        crc_error_q <= crc_error;
        crc_end_q <= crc_end;
    end

    // User JTAG data register (USER0/USER1) and core-driven TAP inputs.
    wire tck, tdi, tms, shift, update, clkdr, runidle, usr1, tdo_core;
    reg [7:0] user_dr = 8'd0;
    reg [3:0] core = 4'd0;
    reg tdo_core_q = 1'b0;
    cyclonev_jtag jtag(
        .tms(altera_reserved_tms), .tck(altera_reserved_tck), .tdi(altera_reserved_tdi), .tdo(altera_reserved_tdo),
        .tdouser(user_dr[0]), .tckutap(tck), .tdiutap(tdi), .tmsutap(tms), .shiftuser(shift), .clkdruser(clkdr),
        .updateuser(update), .runidleuser(runidle), .usr1user(usr1),
        .tckcore(core[0]), .tmscore(core[1]), .tdicore(core[2]), .tdocore(tdo_core), .corectl(SW[0]),
        .ntdopinena(SW[1]));
    always @(posedge FPGA_CLK1_50) begin
        core <= core + 1'b1;
        tdo_core_q <= tdo_core;
    end
    always @(posedge tck) begin
        if (shift)
            user_dr <= {tdi, user_dr[7:1]};
        else if (clkdr)
            user_dr <= {KEY, SW[3:2], usr1, runidle, tms, update};
    end

    // Internal oscillator, both outputs. Sampled as data: the open flow has
    // only two fabric-fed global clock buffers, and the JTAG clock uses one.
    wire osc, osc1;
    cyclonev_oscillator oscillator(.oscena(KEY[0]), .clkout(osc), .clkout1(osc1));
    reg [15:0] osc_count = 16'd0, osc1_count = 16'd0;
    always @(posedge FPGA_CLK1_50) begin
        osc_count <= {osc_count[14:0], osc};
        osc1_count <= {osc1_count[14:0], osc1};
    end

    assign LED = chip_id[7:0] ^ chip_id[15:8] ^ chip_id[23:16] ^ chip_id[31:24] ^ chip_id[39:32] ^
                 chip_id[47:40] ^ chip_id[55:48] ^ chip_id[63:56] ^ syndrome[7:0] ^ syndrome[15:8] ^
                 syndrome[23:16] ^ syndrome[31:24] ^ operation[7:0] ^ operation[15:8] ^ operation[23:16] ^
                 operation[31:24] ^ user_dr ^ {crc_error_q, crc_end_q, tdo_core_q, 5'd0} ^ osc_count[15:8] ^
                 osc1_count[15:8];
endmodule
