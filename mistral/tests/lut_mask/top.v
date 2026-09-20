module top(input clk, input signal, output value, output inverted, output parity, output memory);
    reg [15:0] sequence = 16'hace1;
    always @(posedge clk)
        sequence <= {sequence[14:0], sequence[15] ^ sequence[13] ^ sequence[12] ^ sequence[10]};
    assign value = sequence[15];
    assign inverted = ~sequence[14];
    assign parity = sequence[11] ^ sequence[5];
    reg storage [0:31];
    integer i;
    initial for (i = 0; i < 32; i = i + 1) storage[i] = (32'ha5c39f07 >> i) & 1;
    always @(posedge clk) if (signal) storage[sequence[4:0]] <= sequence[6];
    assign memory = storage[sequence[9:5]];
endmodule
