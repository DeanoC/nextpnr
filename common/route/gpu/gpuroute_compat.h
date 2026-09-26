/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2026  Deano Calver
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 *  Thin compatibility layer so the same kernel source compiles as HIP (ROCm,
 *  or HIP-on-NVIDIA) and as plain CUDA. The kernel is written against the
 *  HIP runtime API; under nvcc the names are mapped to their CUDA equivalents.
 */

#ifndef GPUROUTE_COMPAT_H
#define GPUROUTE_COMPAT_H

#if defined(__HIP_PLATFORM_AMD__) || defined(__HIP_PLATFORM_NVIDIA__) || defined(__HIPCC__) ||                        \
        defined(GPUROUTE_USE_HIP)
#include <hip/hip_runtime.h>
#define GPUROUTE_BACKEND_NAME "hip"
#elif defined(__CUDACC__) || defined(GPUROUTE_USE_CUDA)
#include <cuda_runtime.h>
#define GPUROUTE_BACKEND_NAME "cuda"
#define hipError_t cudaError_t
#define hipSuccess cudaSuccess
#define hipGetErrorString cudaGetErrorString
#define hipGetDeviceCount cudaGetDeviceCount
#define hipSetDevice cudaSetDevice
#define hipDeviceProp_t cudaDeviceProp
#define hipGetDeviceProperties cudaGetDeviceProperties
#define hipMalloc cudaMalloc
#define hipFree cudaFree
#define hipMemcpy cudaMemcpy
#define hipMemset cudaMemset
#define hipMemcpyHostToDevice cudaMemcpyHostToDevice
#define hipMemcpyDeviceToHost cudaMemcpyDeviceToHost
#define hipDeviceSynchronize cudaDeviceSynchronize
#define hipGetLastError cudaGetLastError
#define hipPeekAtLastError cudaPeekAtLastError
#define hipMemGetInfo cudaMemGetInfo
#else
#error "gpuroute_compat.h requires a HIP or CUDA compiler"
#endif

#endif // GPUROUTE_COMPAT_H
