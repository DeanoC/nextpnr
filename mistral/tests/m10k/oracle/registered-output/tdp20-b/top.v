module top(input clka, clkb, we, re, input [8:0] wa, ra, input [19:0] d, output [19:0] qa, qb);
altsyncram #(.intended_device_family("Cyclone V"), .operation_mode("BIDIR_DUAL_PORT"),
.ram_block_type("M10K"), .width_a(20), .widthad_a(9), .numwords_a(512),
.width_b(20), .widthad_b(9), .numwords_b(512), .width_byteena_a(1), .width_byteena_b(1),
.address_reg_a("UNREGISTERED"), .address_reg_b("CLOCK1"),
.outdata_aclr_a("NONE"), .outdata_aclr_b("NONE"), .outdata_reg_a("UNREGISTERED"), .outdata_reg_b("CLOCK1"),
.read_during_write_mode_mixed_ports("DONT_CARE"), .power_up_uninitialized("FALSE")) ram(
.clock0(clka),.clock1(clkb),.clocken0(1'b1),.clocken1(1'b1),.clocken2(1'b1),.clocken3(1'b1),
.address_a(wa),.data_a(d),.wren_a(we),.rden_a(1'b1),.byteena_a(1'b1),.q_a(qa),
.address_b(ra),.q_b(qb),.wren_b(1'b0),.rden_b(re),.byteena_b(1'b1),.data_b(20'b0),
.aclr0(1'b0),.aclr1(1'b0),.addressstall_a(1'b0),.addressstall_b(1'b0));
endmodule
