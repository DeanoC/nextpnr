// Observe the historical controller and its real client, without changing either.
// Ideal clocks only: this records launch/capture opportunities, not silicon slack.
`timescale 1ns/1ps
module legacy_sequence;
parameter integer BASE=0;
reg clk=0, reset=1;
always #5 clk=~clk;
wire start, writing, done, initialized, cs, ras, cas, we, chipclk, oe;
wire [25:0] addr;
wire [15:0] wdata, rdata, dq;
wire [12:0] a;
wire [1:0] ba;
wire busy;
// Return data is irrelevant to command scheduling; the client scans on errors.
mem_channel #(.ADDR_W(26), .WORDS(4), .BASE(BASE)) client (
 .clk(clk), .reset(reset), .stop(1'b0), .start(start), .write(writing),
 .addr(addr), .wdata(wdata), .done(done), .rdata(rdata), .busy(busy),
 .pass(), .fail(), .stopped(), .phase(), .reading(), .shown_addr(),
 .fault_addr(), .last_addr(), .fault_got(), .errors(), .pattern_errors(),
 .shown_expect(), .shown_got());
sdram_addon_port dut (
 .clk(clk), .clk_pin(clk), .rate(2'd2), .reset(reset), .start(start), .write(writing),
 .addr(addr), .wdata(wdata), .write_byte_enable(2'b11), .initialized(initialized),
 .done(done), .rdata(rdata), .sdram_clk(chipclk), .sdram_cke(),
 .sdram_ncs(cs), .sdram_nras(ras), .sdram_ncas(cas), .sdram_nwe(we),
 .sdram_ba(ba), .sdram_a(a), .sdram_dqml(), .sdram_dqmh(),
 .dq_out(dq), .dq_oe(oe), .dq_rise(16'd0), .dq_fall(16'd0));
real changed_a[13], changed_ba[2], changed_dq[16];
real changed_cs=0, changed_ras=0, changed_cas=0, changed_we=0;
reg [12:0] old_a=0;
reg [1:0] old_ba=0;
reg [15:0] old_dq=0;
reg old_cs=0, old_ras=0, old_cas=0, old_we=0, old_oe=0;
// Sample after nonblocking assignments; all controller outputs change at clk.
always @(posedge clk) begin
 #0.001;
 for (integer k=0;k<13;k=k+1)
  if (a[k]!=old_a[k]) changed_a[k]=$realtime-0.001;
 for (integer k=0;k<2;k=k+1)
  if (ba[k]!=old_ba[k]) changed_ba[k]=$realtime-0.001;
 for (integer k=0;k<16;k=k+1)
  if (dq[k]!=old_dq[k]) changed_dq[k]=$realtime-0.001;
 if (cs!=old_cs) changed_cs=$realtime-0.001;
 if (ras!=old_ras) changed_ras=$realtime-0.001;
 if (cas!=old_cas) changed_cas=$realtime-0.001;
 if (we!=old_we) changed_we=$realtime-0.001;
 if (oe!=old_oe && initialized)
  $display("OE time=%.3f value=%0d", $realtime-0.001, oe);
 old_a=a; old_ba=ba; old_dq=dq;
 old_cs=cs; old_ras=ras; old_cas=cas; old_we=we; old_oe=oe;
end
// The board sends cs and its inverse to the two chips: either polarity selects one.
always @(posedge chipclk) if (initialized) begin
 if ((!ras && cas && we) || (ras && !cas)) begin
  $display("CMD time=%.3f kind=%s addr=%0d a=%0h ba=%0d", $realtime,
   !ras ? "ACT" : (we ? "READ" : "WRITE"), addr, a, ba);
  for (integer k=0;k<13;k=k+1)
   $display("AGE port=A bit=%0d ns=%.3f", k, $realtime-changed_a[k]);
  for (integer k=0;k<2;k=k+1)
   $display("AGE port=BA bit=%0d ns=%.3f", k, $realtime-changed_ba[k]);
  $display("AGE port=nCS bit=0 ns=%.3f", $realtime-changed_cs);
  $display("AGE port=nRAS bit=0 ns=%.3f", $realtime-changed_ras);
  $display("AGE port=nCAS bit=0 ns=%.3f", $realtime-changed_cas);
  $display("AGE port=nWE bit=0 ns=%.3f", $realtime-changed_we);
  if (ras && !cas && !we)
   for (integer k=0;k<16;k=k+1)
    $display("AGE port=DQ bit=%0d ns=%.3f", k, $realtime-changed_dq[k]);
 end
end
initial begin
 #40; reset=0; wait(initialized); wait(!busy); #10;
 $display("FINISHED base=%0d", BASE); $finish;
end
initial begin #1000000; $fatal(1,"timeout"); end
endmodule
