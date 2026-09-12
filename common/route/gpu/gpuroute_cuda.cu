// CUDA build of the GPU routing backend. Not exercised on the ROCm
// development machine; the kernel source is shared with gpuroute_hip.hip.
#define GPUROUTE_USE_CUDA 1
#include "gpuroute_kernel.cuh"
