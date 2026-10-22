add_library(nextpnr_place INTERFACE)

target_include_directories(nextpnr_place INTERFACE .)

target_sources(nextpnr_place PUBLIC
    detail_place_cfg.h
    detail_place_core.cc
    detail_place_core.h
    fast_bels.h
    heap_congestion.h
    heap_control_set.h
    heap_spread.h
    heap_assignment.h
    heap_hard_block_cost.h
    heap_hard_blocks.h
    heap_hard_blocks.cc
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
    add_executable(nextpnr-heap-assignment-test tests/heap_assignment.cc)
    target_include_directories(nextpnr-heap-assignment-test PRIVATE .)
    target_link_libraries(nextpnr-heap-assignment-test PRIVATE gtest_main)
    set_property(TARGET nextpnr-heap-assignment-test PROPERTY RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    add_test(NAME nextpnr-heap-assignment-test COMMAND nextpnr-heap-assignment-test)
    add_executable(nextpnr-heap-control-set-test
        tests/heap_control_set.cc
        ../kernel/log.cc
        ../kernel/nextpnr_assertions.cc
    )
    target_include_directories(nextpnr-heap-control-set-test PRIVATE . ../kernel)
    target_link_libraries(nextpnr-heap-control-set-test PRIVATE gtest_main)
    set_property(TARGET nextpnr-heap-control-set-test PROPERTY RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    add_test(NAME nextpnr-heap-control-set-test COMMAND nextpnr-heap-control-set-test)

    add_executable(nextpnr-heap-congestion-test tests/heap_congestion.cc)
    target_include_directories(nextpnr-heap-congestion-test PRIVATE . ../kernel)
    target_link_libraries(nextpnr-heap-congestion-test PRIVATE gtest_main)
    set_property(TARGET nextpnr-heap-congestion-test PROPERTY RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    add_test(NAME nextpnr-heap-congestion-test COMMAND nextpnr-heap-congestion-test)

    add_executable(nextpnr-heap-spread-test tests/heap_spread.cc)
    target_include_directories(nextpnr-heap-spread-test PRIVATE . ../kernel)
    target_link_libraries(nextpnr-heap-spread-test PRIVATE gtest_main)
    set_property(TARGET nextpnr-heap-spread-test PROPERTY RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    add_test(NAME nextpnr-heap-spread-test COMMAND nextpnr-heap-spread-test)
endif()
