module top(input wclk, rclk, we, cer, input [9:0] wa, ra, wd, input [4:0] din, output [9:0] probes);
wire [9:0] qb;
altsyncram #(.intended_device_family("Cyclone V"),.operation_mode("DUAL_PORT"),
.ram_block_type("M10K"),.width_a(10),.widthad_a(10),.numwords_a(1024),
.width_b(10),.widthad_b(10),.numwords_b(1024),.width_byteena_a(1),.width_byteena_b(1),
.address_reg_a("UNREGISTERED"),.address_reg_b("CLOCK1"),
.outdata_reg_a("UNREGISTERED"),.outdata_reg_b("UNREGISTERED"),
.outdata_aclr_a("NONE"),.outdata_aclr_b("NONE"),
.read_during_write_mode_mixed_ports("DONT_CARE"),.power_up_uninitialized("FALSE")) ram(
.clock0(wclk),.clock1(rclk),.clocken0(1'b1),.clocken1(cer),.clocken2(1'b1),.clocken3(1'b1),
.address_a(wa),.address_b(ra),.data_a(wd),.data_b(10'b0),.wren_a(we),.wren_b(1'b0),
.rden_a(1'b1),.rden_b(1'b1),.byteena_a(1'b1),.byteena_b(1'b1),.q_b(qb),
.aclr0(1'b0),.aclr1(1'b0),.addressstall_a(1'b0),.addressstall_b(1'b0));
wire [4:0] launch;
(* preserve, dont_merge *) dffeas launch_0 (.clk(rclk),.d(din[0]),.clrn(1'b1),.prn(1'b1),.ena(1'b1),.asdata(1'b0),.sclr(1'b0),.sload(1'b0),.aload(1'b0),.q(launch[0]));
(* preserve, dont_merge *) dffeas launch_1 (.clk(rclk),.d(din[1]),.clrn(1'b1),.prn(1'b1),.ena(1'b1),.asdata(1'b0),.sclr(1'b0),.sload(1'b0),.aload(1'b0),.q(launch[1]));
(* preserve, dont_merge *) dffeas launch_2 (.clk(rclk),.d(din[2]),.clrn(1'b1),.prn(1'b1),.ena(1'b1),.asdata(1'b0),.sclr(1'b0),.sload(1'b0),.aload(1'b0),.q(launch[2]));
(* preserve, dont_merge *) dffeas launch_3 (.clk(rclk),.d(din[3]),.clrn(1'b1),.prn(1'b1),.ena(1'b1),.asdata(1'b0),.sclr(1'b0),.sload(1'b0),.aload(1'b0),.q(launch[3]));
(* preserve, dont_merge *) dffeas launch_4 (.clk(rclk),.d(din[4]),.clrn(1'b1),.prn(1'b1),.ena(1'b1),.asdata(1'b0),.sclr(1'b0),.sload(1'b0),.aload(1'b0),.q(launch[4]));
wire mixed;
cyclonev_lcell_comb #(.lut_mask(64'h6996966996696996),.shared_arith("off"),.extended_lut("off")) target(
.dataa(launch[0]),.datab(qb[5]),.datac(launch[1]),.datad(launch[2]),.datae(launch[3]),.dataf(launch[4]),
.datag(1'b0),.cin(1'b0),.sharein(1'b0),.combout(mixed));
(* preserve, dont_merge *) dffeas capture(.clk(rclk),.d(mixed),.clrn(1'b1),.prn(1'b1),.ena(1'b1),.asdata(1'b0),.sclr(1'b0),.sload(1'b0),.aload(1'b0),.q(probes[5]));
assign probes[0]=qb[0];
assign probes[1]=qb[1];
assign probes[2]=qb[2];
assign probes[3]=qb[3];
assign probes[4]=qb[4];
assign probes[6]=qb[6];
assign probes[7]=qb[7];
assign probes[8]=qb[8];
assign probes[9]=qb[9];
endmodule
