// Fabric clock with no create_clock. A multicycle on it must not dereference
// a null clock constraint.
module free(input wire clk, input wire d, output reg q);
    reg s;
    always @(posedge clk) begin
        s <= d;
        q <= s;
    end
endmodule
