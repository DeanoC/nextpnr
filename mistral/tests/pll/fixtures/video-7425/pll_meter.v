module pll_meter #(
    parameter WINDOW_BITS = 20
) (
    input wire refclk,
    input wire testclk,
    input wire locked,
    input wire request,
    output reg busy = 0,
    output reg done = 0,
    output reg lost_lock = 0,
    output reg lock_status = 0,
    output reg [15:0] result = 0
);
    // Only one slow signal crosses from the PLL domain. Dividing by 256
    // gives the reference synchronizer ample time to observe every edge.
    reg [7:0] prescale = 0;
    always @(posedge testclk)
        prescale <= prescale + 1'b1;

    (* async_reg = "true" *) reg divided_meta = 0;
    (* async_reg = "true" *) reg divided_sync = 0;
    (* async_reg = "true" *) reg request_meta = 0;
    (* async_reg = "true" *) reg request_sync = 0;
    (* async_reg = "true" *) reg lock_meta = 0;
    reg divided_last = 0;
    reg active_request = 0;
    reg [WINDOW_BITS-1:0] remaining = 0;
    reg [15:0] edges = 0;
    wire rise = divided_sync && !divided_last;

    always @(posedge refclk) begin
        divided_meta <= prescale[7];
        divided_sync <= divided_meta;
        divided_last <= divided_sync;
        request_meta <= request;
        request_sync <= request_meta;
        lock_meta <= locked;
        lock_status <= lock_meta;
        if (!busy && request_sync != done) begin
            active_request <= request_sync;
            busy <= 1;
            remaining <= {WINDOW_BITS{1'b1}};
            edges <= 0;
            lost_lock <= !lock_status;
        end else if (busy) begin
            edges <= edges + {15'b0, rise};
            lost_lock <= lost_lock || !lock_status;
            remaining <= remaining - 1'b1;
            if (remaining == 0) begin
                result <= edges + {15'b0, rise};
                done <= active_request;
                busy <= 0;
            end
        end
    end
endmodule
