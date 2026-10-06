// Diagnostic ideal-clock, single-word return model; not a silicon model.
`timescale 1ns/1ps
module trace;
parameter integer RATE=100;
localparam real T=1000.0/RATE;
localparam real CAP=RATE==100 ? 5.0 : 6.538;
localparam integer CL=RATE==100 ? 2 : 3;
reg clk=0, cap=0, reset=1, start=0;
reg [15:0] pin=16'hdead, sample=16'hdead, held=16'hdead;
wire done, initialized, cs, ras, cas, we, chipclk;
wire [15:0] rdata;
real sample_time=0, held_time=0, read_time=-1, consumed_time=0;
real tac=1.0, return_delay=0.0;
initial begin
 if ($value$plusargs("tac=%f",tac)) begin end
 if ($value$plusargs("return_delay=%f",return_delay)) begin end
end
always #(T/2) clk=~clk;
initial begin #(CAP); forever #(T/2) cap=~cap; end
always @(posedge cap) begin sample<=pin; sample_time=$realtime; end
always @(negedge clk) begin held<=sample; held_time=sample_time; end
wire [15:0] capture_word=RATE==100 ? sample : held;
sdram_addon_port #(.IO_OUTPUT_REGISTERS(1)) dut(
 .clk(clk),.clk_pin(clk),.rate(RATE==100 ? 2'd2 : 2'd1),.reset(reset),.start(start),.write(1'b0),
 .addr(26'd0),.wdata(16'd0),.write_byte_enable(2'b11),.initialized(initialized),.done(done),.rdata(rdata),
 .sdram_clk(chipclk),.sdram_cke(),.sdram_ncs(cs),.sdram_nras(ras),.sdram_ncas(cas),.sdram_nwe(we),
 .sdram_ba(),.sdram_a(),.sdram_dqml(),.sdram_dqmh(),.dq_out(),.dq_oe(),.dq_rise(capture_word),.dq_fall(capture_word));
always @(posedge chipclk) if (!cs && ras && !cas && we) begin
 read_time=$realtime;
 $display("READ rate=%0d time=%.3f launch=%.3f", RATE,read_time,read_time+CL*T);
 fork begin #(CL*T+return_delay+tac); pin=16'hbeef; #(T+2.5-tac); pin=16'hdead; end join_none
end
always @(posedge clk) if (read_time>=0 && $realtime<read_time+10*T) begin
 if ((RATE==100 && dut.state==12 && dut.wait_count==3) ||
     (RATE==130 && dut.capture_due))
   consumed_time=RATE==100 ? sample_time : held_time;
 $display("EDGE time=%.3f state=%0d wait=%0d due=%0d sample_at=%.3f held_at=%.3f dq=%h",$realtime,dut.state,dut.wait_count,dut.capture_due,sample_time,held_time,capture_word);
end
initial begin
 #(4*T); reset=0; wait(initialized); @(negedge clk); start=1;
 wait(done); #0.001; $display("RESULT rate=%0d data=%h capture=%.3f launch=%.3f valid_from=%.3f valid_until=%.3f",RATE,rdata,consumed_time,read_time+CL*T,read_time+CL*T+return_delay+tac,read_time+(CL+1)*T+return_delay+2.5); $finish;
end
initial begin #(1000000); $fatal(1,"timeout");end
endmodule
