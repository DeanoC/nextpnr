# 50 MHz DE10-Nano input clock. nextpnr-mistral derives 52.224 MHz
# (normal) or 56 MHz (fast) and 74.25 MHz video from altera_pll cells.
# Only normal mode has a 12.288 MHz audio domain; fast audio uses clk_sys
# enables and pin-only rational MCLK. The producer gates actual domains.
create_clock -name FPGA_CLK1_50 -period 20.000 [get_ports {FPGA_CLK1_50}]
