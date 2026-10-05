// DDR capture on bidirectional pads, the RAM tester's SDRAM DQ pattern
// (altiobuf_bidir + altddio_in), with packed output and OE registers.
module top(input wire clk, input wire [1:0] d, input wire oe, inout wire [1:0] dq, output wire [3:0] q);
    reg [1:0] dq_out; reg dq_oe;
    always @(posedge clk) begin dq_out <= d; dq_oe <= oe; end
    genvar i;
    generate for (i = 0; i < 2; i = i + 1) begin : pads
        wire pin;
        altiobuf_bidir #(.number_of_channels(1), .enable_bus_hold("FALSE")) pad (
            .dataio(dq[i]), .oe(dq_oe), .datain(dq_out[i]), .dataout(pin));
        altddio_in #(.width(1), .intended_device_family("Cyclone V"), .power_up_high("OFF"),
                     .invert_input_clocks("OFF")) cap (
            .datain(pin), .inclock(clk), .inclocken(1'b1), .aset(1'b0), .aclr(1'b0), .sset(1'b0), .sclr(1'b0),
            .dataout_h(q[2*i+1]), .dataout_l(q[2*i]));
    end endgenerate
endmodule
