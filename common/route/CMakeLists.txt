add_library(nextpnr_route INTERFACE)

target_include_directories(nextpnr_route INTERFACE .)

target_sources(nextpnr_route PUBLIC
    router1.cc
    router1.h
    router2.cc
    router2.h
    gpurouter.cc
    gpurouter.h
    gpu/gpuroute_backend.h
    gpu/gpuroute_cpu.cc
)

# GPU device backend for the GPU router. The device code is compiled by the
# vendor compiler into a static library with a plain C++ interface
# (gpu/gpuroute_backend.h) so the rest of nextpnr keeps its normal compiler.
if (GPU_ROUTER STREQUAL "HIP")
    if (NOT DEFINED CMAKE_HIP_ARCHITECTURES)
        set(CMAKE_HIP_ARCHITECTURES "gfx1100;gfx1201" CACHE STRING "HIP offload architectures")
    endif()
    if (EXISTS "/opt/rocm" AND NOT ROCM_PATH)
        set(ROCM_PATH "/opt/rocm")
    endif()
    if (ROCM_PATH)
        list(APPEND CMAKE_PREFIX_PATH "${ROCM_PATH}")
    endif()
    enable_language(HIP)
    find_package(hip REQUIRED)
    add_library(nextpnr_gpuroute_device STATIC gpu/gpuroute_hip.hip)
    target_include_directories(nextpnr_gpuroute_device PRIVATE gpu)
    target_link_libraries(nextpnr_gpuroute_device PUBLIC hip::host)
    target_link_libraries(nextpnr_route INTERFACE nextpnr_gpuroute_device)
    target_compile_definitions(nextpnr_route INTERFACE NPNR_GPU_ROUTER_DEVICE=1)
    message(STATUS "GPU router: HIP device backend for ${CMAKE_HIP_ARCHITECTURES}")
elseif (GPU_ROUTER STREQUAL "CUDA")
    enable_language(CUDA)
    find_package(CUDAToolkit REQUIRED)
    add_library(nextpnr_gpuroute_device STATIC gpu/gpuroute_cuda.cu)
    target_include_directories(nextpnr_gpuroute_device PRIVATE gpu)
    target_link_libraries(nextpnr_gpuroute_device PUBLIC CUDA::cudart)
    target_link_libraries(nextpnr_route INTERFACE nextpnr_gpuroute_device)
    target_compile_definitions(nextpnr_route INTERFACE NPNR_GPU_ROUTER_DEVICE=1)
    message(STATUS "GPU router: CUDA device backend")
elseif (GPU_ROUTER STREQUAL "OFF" OR NOT GPU_ROUTER)
    target_sources(nextpnr_route PUBLIC gpu/gpuroute_nodevice.cc)
    message(STATUS "GPU router: no device backend (CPU reference only); set -DGPU_ROUTER=HIP or CUDA")
else()
    message(FATAL_ERROR "GPU_ROUTER must be OFF, HIP or CUDA (got '${GPU_ROUTER}')")
endif()
