from pathlib import Path
pins='V12 E8 D11 W12 AH13 D8 AH14 AF7 AE24 AD23 AE6 AE23 AG14 AD5 AF4 AH3'.split()
for load in (0,30):
 p=Path(f'dq-native{load}');p.mkdir(exist_ok=True)
 (p/'top.v').write_text('''module top(input clk, input seed, inout [15:0] dq, output reg alive);
reg [15:0] data, oe, capture;
always @(posedge clk) begin
 data <= {data[14:0],seed}; oe <= {oe[14:0],data[15]}; capture <= dq; alive <= ^capture;
end
genvar i; generate for(i=0;i<16;i=i+1) begin: pads
 assign dq[i] = oe[i] ? data[i] : 1'bz;
end endgenerate
endmodule
''')
 (p/'top.qpf').write_text('PROJECT_REVISION = "top"\n')
 q='''set_global_assignment -name FAMILY "Cyclone V"
set_global_assignment -name DEVICE 5CSEBA6U23I7
set_global_assignment -name TOP_LEVEL_ENTITY top
set_global_assignment -name VERILOG_FILE top.v
set_global_assignment -name SDC_FILE clocks.sdc
set_global_assignment -name PROJECT_OUTPUT_DIRECTORY output_files
set_global_assignment -name GENERATE_RBF_FILE ON
set_global_assignment -name NUM_PARALLEL_PROCESSORS 2
set_global_assignment -name STRATIX_DEVICE_IO_STANDARD "3.3-V LVTTL"
set_location_assignment PIN_V11 -to clk
set_location_assignment PIN_W15 -to seed
set_location_assignment PIN_Y11 -to alive
'''
 for i,pin in enumerate(pins):
  n=f'dq[{i}]';q+=f'set_location_assignment PIN_{pin} -to {n}\n'
  for k,v in [('FAST_INPUT_REGISTER','OFF'),('FAST_OUTPUT_REGISTER','OFF'),('FAST_OUTPUT_ENABLE_REGISTER','OFF'),('D1_DELAY',0),('D3_DELAY',0),('D5_DELAY',0),('D5_OE_DELAY',31),('CURRENT_STRENGTH_NEW','16MA'),('SLEW_RATE',1),('BOARD_MODEL_FAR_C',load*1e-12)]:q+=f'set_instance_assignment -name {k} {v} -to {n}\n'
 (p/'top.qsf').write_text(q)
 (p/'clocks.sdc').write_text('create_clock -period 20 -name clock [get_ports clk]\nset_input_delay -clock clock -min 0 [get_ports {dq[*] seed}]\nset_input_delay -clock clock -max 1 [get_ports {dq[*] seed}]\nset_output_delay -clock clock -min 0 [get_ports {dq[*] alive}]\nset_output_delay -clock clock -max 1 [get_ports {dq[*] alive}]\n')
