// Two independent shift registers with a tail, so PLACE_REGION tests can
// fence each hierarchy and override part of one with a more specific target.
module stage (input wire clk, input wire d, output wire q);
    reg [7:0] body;
    reg [3:0] tail;
    always @(posedge clk) begin
        body <= {body[6:0], d ^ body[7]};
        tail <= {tail[2:0], body[7] & body[3]};
    end
    assign q = tail[3];
endmodule

module top (input wire clk, input wire a, input wire b, output wire qa, output wire qb);
    stage left (.clk(clk), .d(a), .q(qa));
    stage right (.clk(clk), .d(b), .q(qb));
endmodule
