// Quartus assignment precedence: wildcard, then whole bus, then exact name.
module top(input wire [3:0] d, output wire [3:0] q);
    assign q = ~d;
endmodule
