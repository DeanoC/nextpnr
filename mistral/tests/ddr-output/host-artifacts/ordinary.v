module top(input FPGA_CLK1_50, output DDR_OUT);
 wire [31:0] gpo;
 reg [15:0] beat=0;
 always @(posedge FPGA_CLK1_50) beat<=beat+1'b1;
 cyclonev_hps_interface_mpu_general_purpose gp(.gp_in({16'hDD01,beat}),.gp_out(gpo));
 assign DDR_OUT=beat[15];
endmodule
