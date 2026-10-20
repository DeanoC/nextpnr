from pathlib import Path
import re
root=Path(__file__).resolve().parent
import sys
repo=Path(__file__).resolve().parents[3]
root=Path(sys.argv[1]).resolve()
root.mkdir(parents=True, exist_ok=True)
base=(repo/'mistral/tests/gpio-timing/ramtest-pads.qsf').read_text()
rtl='''module top(input FPGA_CLK1_50, output reg [10:0] SDRAM_A, output reg [1:0] SDRAM_BA);
reg [15:0] beat = 0;
always @(posedge FPGA_CLK1_50) begin
 beat <= beat + 1'b1;
 if (beat[15]) begin SDRAM_A <= beat[10:0]; SDRAM_BA <= beat[12:11]; end
end
endmodule
'''
pins=re.findall(r'^set_location_assignment PIN_(\w+) -to (SDRAM_(?:A|BA)\[\d+\])$',base,re.M)
pins=[(pin,p) for pin,p in pins if p not in ['SDRAM_A[11]','SDRAM_A[12]']]
for load in [0,30]:
 p=root/f'reference{load}'
 p.mkdir(exist_ok=True)
 qsf='\n'.join(l for l in base.splitlines() if l.startswith('set_global_assignment') or '-to FPGA_CLK1_50' in l).replace('ramtest-pads.v','top.v')+'\n'
 for pin,port in pins:
  qsf+=f'set_location_assignment PIN_{pin} -to {port}\n'
  for name,val in [('FAST_OUTPUT_REGISTER','ON'),('D5_DELAY','0'),('CURRENT_STRENGTH_NEW','"16MA"'),('SLEW_RATE','1'),('BOARD_MODEL_FAR_C',str(load*1e-12))]:
   qsf+=f'set_instance_assignment -name {name} {val} -to {port}\n'
 (p/'top.v').write_text(rtl)
 (p/'top.qsf').write_text(qsf)
 (p/'top.qpf').write_text('PROJECT_REVISION = "top"\n')
 (p/'clocks.sdc').write_text('create_clock -period 10 -name memory [get_ports FPGA_CLK1_50]\nset_output_delay -clock memory -min 0 [all_outputs]\nset_output_delay -clock memory -max 1 [all_outputs]\n')
 query=(repo/'mistral/tests/gpio-timing/ramtest-paths.tcl').read_text()
 query=query.replace('        report_timing -$kind -from [get_ports {SDRAM_DQ[*]}] -npaths 500 -nworst 100 -detail full_path -file $corner/input-$kind.rpt\n','').replace('        report_timing -$kind -to [get_ports {q[*]}] -npaths 500 -nworst 100 -detail full_path -file $corner/fabric-$kind.rpt','''        foreach transition {rise fall} {
            report_timing -$kind -${transition}_to [get_ports {SDRAM_*}] -npaths 500 -nworst 100 -detail full_path -file $corner/output-$transition-$kind.rpt
        }''')
 (p/'paths.tcl').write_text(query)
 (p/'clock_requirements.tcl').write_text((repo/'mistral/tests/gpio-timing/clock_requirements.tcl').read_text())
