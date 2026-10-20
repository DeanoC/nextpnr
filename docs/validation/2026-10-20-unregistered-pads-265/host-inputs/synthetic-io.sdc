# HOST-ONLY SYNTHETIC TEST BUDGETS. NOT PHYSICAL INTERFACE CONSTRAINTS.
# Frozen checkpoint and all saved physical settings are unchanged.
set_input_delay -clock {clocks[0]} -clock_fall -min 1 [get_ports {SDRAM_DQ[*]}]
set_input_delay -clock {clocks[0]} -clock_fall -max 3 [get_ports {SDRAM_DQ[*]}]
set_output_delay -clock {clocks[0]} -min -1 [get_ports {SDRAM_DQ[*] SDRAM_DQML SDRAM_DQMH}]
set_output_delay -clock {clocks[0]} -max 2 [get_ports {SDRAM_DQ[*] SDRAM_DQML SDRAM_DQMH}]
set_output_delay -clock {clk_pixel} -min -1 [get_ports {HDMI_TX_D[*] HDMI_TX_VS HDMI_TX_HS HDMI_TX_DE HDMI_TX_CLK}]
set_output_delay -clock {clk_pixel} -max 2 [get_ports {HDMI_TX_D[*] HDMI_TX_VS HDMI_TX_HS HDMI_TX_DE HDMI_TX_CLK}]
set_output_delay -clock {clocks[0]} -min -1 [get_ports {HDMI_I2C_SDA HDMI_I2C_SCL}]
set_output_delay -clock {clocks[0]} -max 2 [get_ports {HDMI_I2C_SDA HDMI_I2C_SCL}]
