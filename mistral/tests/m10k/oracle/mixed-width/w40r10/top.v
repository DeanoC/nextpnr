module top(input clka, clkb, we, re, input [7:0] wa, input [9:0] ra,
 input [39:0] d, output [9:0] q);
 altsyncram #(.intended_device_family("Cyclone V"), .operation_mode("DUAL_PORT"),
 .ram_block_type("M10K"), .width_a(40), .widthad_a(8), .numwords_a(256),
 .width_b(10), .widthad_b(10), .numwords_b(1024),
 .address_reg_b("CLOCK1"), .outdata_reg_b("UNREGISTERED"),
 .read_during_write_mode_mixed_ports("DONT_CARE"), .power_up_uninitialized("FALSE")) ram(
 .clock0(clka), .clock1(clkb), .clocken0(1'b1), .clocken1(re), .clocken2(1'b1), .clocken3(1'b1),
 .address_a(wa), .data_a(d), .wren_a(we), .rden_a(1'b1), .byteena_a(1'b1),
 .address_b(ra), .q_b(q), .wren_b(1'b0), .rden_b(1'b1), .byteena_b(1'b1), .data_b(10'b0),
 .aclr0(1'b0), .aclr1(1'b0), .addressstall_a(1'b0), .addressstall_b(1'b0));
endmodule
