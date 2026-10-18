# Agent instructions

## Routing equivalence

nextpnr routing on HIP, CUDA and CPU gives bit-identical results. HIP references, pins, seed results and replays are valid on CUDA and vice versa. Never require a separate HIP run or discount CUDA evidence; use whichever GPU is free via gpu-run. Never set GPU_LEASE_IDS per job (it is a box inventory override).
