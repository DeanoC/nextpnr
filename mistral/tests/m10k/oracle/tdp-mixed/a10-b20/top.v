module top(input clka, clkb, wea, web, ena, enb,
 input [9:0] aa, input [8:0] ab,
 input [9:0] da, input [19:0] db, output [9:0] qa, output [19:0] qb);
 altsyncram #(.intended_device_family("Cyclone V"), .operation_mode("BIDIR_DUAL_PORT"),
 .ram_block_type("M10K"), .width_a(10), .widthad_a(10), .numwords_a(1024),
 .width_b(20), .widthad_b(9), .numwords_b(512),
 .address_reg_b("CLOCK1"), .indata_reg_b("CLOCK1"), .wrcontrol_wraddress_reg_b("CLOCK1"),
 .outdata_reg_a("UNREGISTERED"), .outdata_reg_b("UNREGISTERED"),
 .read_during_write_mode_port_a("NEW_DATA_NO_NBE_READ"), .read_during_write_mode_port_b("NEW_DATA_NO_NBE_READ"),
 .read_during_write_mode_mixed_ports("DONT_CARE"), .power_up_uninitialized("FALSE")) ram(
 .clock0(clka), .clock1(clkb), .clocken0(ena), .clocken1(enb), .clocken2(1'b1), .clocken3(1'b1),
 .address_a(aa), .data_a(da), .q_a(qa), .wren_a(wea), .rden_a(1'b1), .byteena_a(1'b1),
 .address_b(ab), .data_b(db), .q_b(qb), .wren_b(web), .rden_b(1'b1), .byteena_b(1'b1),
 .aclr0(1'b0), .aclr1(1'b0), .addressstall_a(1'b0), .addressstall_b(1'b0));
endmodule
