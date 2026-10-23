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
    gpu/gpuroute_lease.cc
    gpu/gpuroute_lease.h
    gpu/gpuroute_telemetry.cc
    gpu/gpuroute_telemetry.h
)

if (BUILD_TESTS)
    add_executable(nextpnr-gpuroute-telemetry-test
        tests/gpuroute_telemetry.cc
        gpu/gpuroute_telemetry.cc
    )
    target_include_directories(nextpnr-gpuroute-telemetry-test PRIVATE gpu)
    target_link_libraries(nextpnr-gpuroute-telemetry-test PRIVATE gtest_main json11)
    set_property(TARGET nextpnr-gpuroute-telemetry-test PROPERTY RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    add_test(NAME nextpnr-gpuroute-telemetry-test COMMAND nextpnr-gpuroute-telemetry-test)

    add_executable(nextpnr-reserved-route-slack-test
        tests/reserved_route_slack.cc
    )
    target_include_directories(nextpnr-reserved-route-slack-test PRIVATE gpu)
    target_link_libraries(nextpnr-reserved-route-slack-test PRIVATE gtest_main)
    set_property(TARGET nextpnr-reserved-route-slack-test PROPERTY RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    add_test(NAME nextpnr-reserved-route-slack-test COMMAND nextpnr-reserved-route-slack-test)

    add_executable(nextpnr-gpu-lease-test
        tests/gpu_lease.cc
        gpu/gpuroute_lease.cc
    )
    target_include_directories(nextpnr-gpu-lease-test PRIVATE gpu)
    target_link_libraries(nextpnr-gpu-lease-test PRIVATE gtest_main)
    if (Threads_FOUND)
        target_link_libraries(nextpnr-gpu-lease-test PRIVATE Threads::Threads)
    endif()
    set_property(TARGET nextpnr-gpu-lease-test PROPERTY RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    add_test(NAME nextpnr-gpu-lease-test COMMAND nextpnr-gpu-lease-test)
endif()

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
    # No fused multiply-add: the host backend evaluates the same cost
    # expressions and must reach bit-identical values
    target_compile_options(nextpnr_gpuroute_device PRIVATE -ffp-contract=off)
    target_link_libraries(nextpnr_gpuroute_device PUBLIC hip::host)
    target_link_libraries(nextpnr_route INTERFACE nextpnr_gpuroute_device)
    target_compile_definitions(nextpnr_route INTERFACE NPNR_GPU_ROUTER_DEVICE=1)
    message(STATUS "GPU router: HIP device backend for ${CMAKE_HIP_ARCHITECTURES}")
elseif (GPU_ROUTER STREQUAL "CUDA")
    enable_language(CUDA)
    find_package(CUDAToolkit REQUIRED)
    add_library(nextpnr_gpuroute_device STATIC gpu/gpuroute_cuda.cu)
    target_include_directories(nextpnr_gpuroute_device PRIVATE gpu)
    target_compile_options(nextpnr_gpuroute_device PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:--fmad=false>)
    target_link_libraries(nextpnr_gpuroute_device PUBLIC CUDA::cudart)
    target_link_libraries(nextpnr_route INTERFACE nextpnr_gpuroute_device)
    target_compile_definitions(nextpnr_route INTERFACE NPNR_GPU_ROUTER_DEVICE=1)
    message(STATUS "GPU router: CUDA device backend")
elseif (GPU_ROUTER STREQUAL "METAL")
    if (NOT APPLE)
        message(FATAL_ERROR "GPU_ROUTER=METAL needs macOS")
    endif()
    enable_language(OBJCXX)
    # The kernel is compiled when the backend opens a device, so the build
    # needs only the Metal framework. Embed its source as a string; editing
    # it re-runs CMake.
    set(GPUROUTE_METAL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/gpu/gpuroute_metal.metal)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${GPUROUTE_METAL_SRC})
    file(READ ${GPUROUTE_METAL_SRC} GPUROUTE_METAL_TEXT)
    file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gpu/gpuroute_metal_source.h
        CONTENT "static const char gpuroute_metal_source[] = R\"GPUROUTE_MSL(@GPUROUTE_METAL_TEXT@)GPUROUTE_MSL\";\n"
        @ONLY)
    add_library(nextpnr_gpuroute_device STATIC gpu/gpuroute_metal.mm)
    target_include_directories(nextpnr_gpuroute_device PRIVATE gpu ${CMAKE_CURRENT_BINARY_DIR}/gpu)
    target_compile_options(nextpnr_gpuroute_device PRIVATE -fobjc-arc)
    target_link_libraries(nextpnr_gpuroute_device PUBLIC "-framework Metal" "-framework Foundation")
    target_link_libraries(nextpnr_route INTERFACE nextpnr_gpuroute_device)
    target_compile_definitions(nextpnr_route INTERFACE NPNR_GPU_ROUTER_DEVICE=1)
    message(STATUS "GPU router: Metal device backend")
elseif (GPU_ROUTER STREQUAL "OFF" OR NOT GPU_ROUTER)
    target_sources(nextpnr_route PUBLIC gpu/gpuroute_nodevice.cc)
    message(STATUS "GPU router: no device backend (CPU reference only); set -DGPU_ROUTER=HIP, CUDA or METAL")
else()
    message(FATAL_ERROR "GPU_ROUTER must be OFF, HIP, CUDA or METAL (got '${GPU_ROUTER}')")
endif()
