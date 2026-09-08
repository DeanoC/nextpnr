module top(input clka, clkb, we, re, input [7:0] wa, ra, input [39:0] d, output reg [39:0] q);
(* ramstyle = "M10K, no_rw_check" *) reg [39:0] mem[0:255];
integer i;
initial for (i=0; i<256; i=i+1) mem[i] = ((i*73)^(i>>1)^40'h12345a65b9)&40'hffffffffff;
always @(posedge clka) if (we) mem[wa] <= d;
always @(posedge clkb) if (re) q <= mem[ra];
endmodule
