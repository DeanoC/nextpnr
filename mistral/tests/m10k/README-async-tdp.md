# Cyclone V M10K true-dual asynchronous reads

Cyclone V M10K registers both read addresses, including in true-dual-port
mode. `CFG_ASYNC_READ=1` on `MISTRAL_M10K_TDP` cannot implement a
combinational read and is rejected before placement. The true-dual fixture is
covered by [the asynchronous read regression](README-async-read.md).
