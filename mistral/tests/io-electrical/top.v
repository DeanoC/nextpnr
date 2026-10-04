// Every checked 3.3 V I/O electrical option on one design. Quartus infers the
// tri-state pads; the open flow instantiates MISTRAL_IO explicitly.
module top(
    input  wire clk,
    input  wire i_plain, i_pullup, i_bushold, i_clamp, i_lvcmos, i_d3, i_d1reg,
    output wire o_plain, o_4ma, o_8ma, o_min, o_slow, o_lvcmos, o_clamp, o_pullup, o_bushold,
    output wire o_d5, o_d5max, o_reg_d5,
    inout  wire b_d3_d5, b_slow4, b_pullup,
    output wire t_oe
);
    reg [15:0] r;
    reg in_q, out_q;
    wire b_d3_d5_in, b_slow4_in, b_pullup_in;
    always @(posedge clk) begin
        in_q <= i_d1reg;
        out_q <= r[15];
        r <= {i_plain, i_pullup, i_bushold, i_clamp, i_lvcmos, i_d3, in_q,
              b_d3_d5_in, b_slow4_in, b_pullup_in, r[15:10]} ^ r;
    end
    assign o_plain = r[0];
    assign o_4ma = r[1];
    assign o_8ma = r[2];
    assign o_min = r[3];
    assign o_slow = r[4];
    assign o_lvcmos = r[5];
    assign o_clamp = r[6];
    assign o_pullup = r[7];
    assign o_bushold = r[8];
    assign o_d5 = r[9];
    assign o_d5max = r[10];
    assign o_reg_d5 = out_q;
`ifdef QUARTUS
    assign b_d3_d5 = r[11] ? r[12] : 1'bz;
    assign b_slow4 = r[12] ? r[13] : 1'bz;
    assign b_pullup = r[13] ? r[14] : 1'bz;
    assign t_oe = r[14] ? r[15] : 1'bz;
    assign b_d3_d5_in = b_d3_d5;
    assign b_slow4_in = b_slow4;
    assign b_pullup_in = b_pullup;
`else
    MISTRAL_IO b_d3_d5_pad (.PAD(b_d3_d5), .I(r[12]), .OE(r[11]), .O(b_d3_d5_in));
    MISTRAL_IO b_slow4_pad (.PAD(b_slow4), .I(r[13]), .OE(r[12]), .O(b_slow4_in));
    MISTRAL_IO b_pullup_pad (.PAD(b_pullup), .I(r[14]), .OE(r[13]), .O(b_pullup_in));
    MISTRAL_IO t_oe_pad (.PAD(t_oe), .I(r[15]), .OE(r[14]));
`endif
endmodule
