add_library(nextpnr_place INTERFACE)

target_include_directories(nextpnr_place INTERFACE .)

target_sources(nextpnr_place PUBLIC
    detail_place_cfg.h
    detail_place_core.cc
    detail_place_core.h
    fast_bels.h
    heap_control_set.h
    parallel_refine.cc
    parallel_refine.h
    place_common.cc
    place_common.h
    placer1.cc
    placer1.h
    placer_heap.cc
    placer_heap.h
    placer_static.cc
    placer_static.h
    static_util.h
    timing_opt.cc
    timing_opt.h
)

if (BUILD_TESTS)
    add_executable(nextpnr-heap-control-set-test
        tests/heap_control_set.cc
        ../kernel/log.cc
        ../kernel/nextpnr_assertions.cc
    )
    target_include_directories(nextpnr-heap-control-set-test PRIVATE . ../kernel)
    target_link_libraries(nextpnr-heap-control-set-test PRIVATE gtest_main)
    set_property(TARGET nextpnr-heap-control-set-test PROPERTY RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    add_test(NAME nextpnr-heap-control-set-test COMMAND nextpnr-heap-control-set-test)
endif()
