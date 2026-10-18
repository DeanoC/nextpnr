# Agent instructions

## Routing equivalence

The GPU router's HIP and CUDA backends and its CPU reference backend give bit-identical route results. HIP, CUDA and CPU-reference route evidence is interchangeable for seed and replay comparisons; this does not combine the distinct router2/router1-fallback cohort. Never require a separate HIP run or discount CUDA evidence; use whichever GPU is free via gpu-run. Never set GPU_LEASE_IDS per job (it is a box inventory override).
