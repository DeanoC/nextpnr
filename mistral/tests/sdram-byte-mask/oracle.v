// Quartus 17.0.2 counterpart, with fabric registers on the same DQM pads.
module top(input clk, clear_masks, lower_data, upper_data, output dqml, dqmh);
    wire [1:0] d;
    wire sclr;
`ifdef MASK_PLAIN_D
    // Keep the mux on D, with inactive dedicated synchronous controls.
    lcell lower_lut (.in(lower_data & !clear_masks), .out(d[0]));
    lcell upper_lut (.in(upper_data & !clear_masks), .out(d[1]));
    assign sclr = 1'b0;
`else
    assign d = {upper_data, lower_data};
    assign sclr = clear_masks;
`endif
    dffeas lower_ff (.d(d[0]), .clk(clk), .ena(1'b1), .clrn(1'b1), .prn(1'b1),
        .sclr(sclr), .sload(1'b0), .asdata(1'b0), .aload(1'b0), .q(dqml));
    dffeas upper_ff (.d(d[1]), .clk(clk), .ena(1'b1), .clrn(1'b1), .prn(1'b1),
        .sclr(sclr), .sload(1'b0), .asdata(1'b0), .aload(1'b0), .q(dqmh));
endmodule
