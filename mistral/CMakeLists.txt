set(SOURCES
    analogue.cc
    arch.cc
    archdefs.cc
    archdefs.h
    arch.h
    arch_pybindings.cc
    arch_pybindings.h
    bitstream.cc
    constids.inc
    ctrl.cc
    delay.cc
    ff_core.cc
    local_remap.cc
    local_remap_pin_policy.h
    remap_plan.cc
    remap_report.h
    comb_remap.cc
    decomposition_remap.cc
    decomposition_policy.h
    lut_driver_copy.cc
    lut_driver_copy.h
    lut_pair_placement.cc
    lut_pair_copy.cc
    lut_pair_placement.h
    remap_clock_guard.h
    comb_plan.cc
    local_remap_cut_policy.h
    capture_locality.cc
    capture_pipeline_locality.cc
    critical_cohort.cc
    critical_cohort.h
    critical_cohort_model.cc
    critical_cohort_model.h
    reduction_balance.cc
    placed_reduction.cc
    reduction_balance_plan.h
    enable_replication.cc
    placed_timeout.cc
    retained_enable.cc
    dsp.h
    dsp.cc
    fes_slot.cc
    globals.cc
    io.cc
    lab.cc
    m10k.cc
    pack.cc
    pins.cc
    qsf.cc
)

add_nextpnr_architecture(${family}
    CORE_SOURCES ${SOURCES}
    TEST_SOURCES tests/gpu_backend_selection.cc tests/analogue_jobs.cc tests/critical_cohort.cc tests/analogue_budget.cc tests/analogue_improve.cc tests/bel_id.cc tests/completed_observations.cc tests/lab_pinmap.cc tests/lab_ff4.cc tests/device_model_lifetime.cc tests/fes_cram.cc tests/fes_boundary.cc tests/fes_narrow_pin.cc tests/enable_replication/backend.cc tests/placed_timeout.cc tests/local_remap.cc tests/placed_reduction.cc tests/local_remap_cut_policy.cc tests/comb_remap.cc tests/remap_plan.cc tests/comb_remap_plan.cc tests/decomposition_policy.cc tests/decomposition_backend.cc tests/lut_driver_copy.cc tests/lut_pair_placement.cc tests/timing_report_paths.cc tests/capture_pipeline_locality.cc tests/io_delay.cc tests/m10k_timing.cc tests/ff_reset_timing.cc tests/ff_core_timing.cc tests/hps_interrupts.cc tests/hps_clocks_resets.cc tests/hps_lw_bridge.cc
    tests/parked_reviews.cc tests/lut_input_delay.cc
    MAIN_SOURCE  main.cc
)

if (BUILD_TESTS)
    add_test(NAME nextpnr-${family}-ff-reset-reference-parser
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ff-reset/test_reference.py)
    add_test(NAME nextpnr-${family}-ff-reset-characterizer
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ff-reset/test_characterize.py)
    add_test(NAME nextpnr-${family}-ff-reset-reference-cli
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ff-reset/check_cli.py
                $<TARGET_FILE:nextpnr-${family}>)
    find_program(MISTRAL_CHECKPOINT_YOSYS yosys)
    add_test(NAME nextpnr-${family}-pip-rejection
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../common/route/tests/mistral_pip_rejection.py)
    add_test(NAME nextpnr-${family}-pad-clock
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/pad_clock.py)
    add_test(NAME nextpnr-${family}-sdrin-path-coverage
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ramtest-io/path_coverage.py --self-check)
    add_test(NAME nextpnr-${family}-sdram-constraint-coverage
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ramtest-io/constraint_coverage.py --self-check)
    add_test(NAME nextpnr-${family}-ramtest-normal-build
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ramtest-io/normal_build.py --self-check)
    add_test(NAME nextpnr-${family}-ramtest-legacy-screen
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ramtest-io/legacy_screen.py --self-check)
    add_test(NAME nextpnr-${family}-critical-cohort-cli
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/critical_cohort_cli.py
                $<TARGET_FILE:nextpnr-${family}> -v)
    if (MISTRAL_CHECKPOINT_YOSYS)
        add_test(NAME nextpnr-${family}-place-region
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/place-region/check.py
                    --yosys ${MISTRAL_CHECKPOINT_YOSYS} $<TARGET_FILE:nextpnr-${family}>)
    endif()
    add_test(NAME nextpnr-${family}-command-settings-cli
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/command_settings_cli.py
                $<TARGET_FILE:nextpnr-${family}>)
    add_test(NAME nextpnr-${family}-gpu-lease-cli
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu_lease_cli.py
                $<TARGET_FILE:nextpnr-${family}>)
    foreach(LUT6_SELECT_FIXTURE asymmetric asymmetric-bottom)
        add_test(NAME nextpnr-${family}-lut6-select-${LUT6_SELECT_FIXTURE}
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/lut6-select/check.py
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/lut6-select/${LUT6_SELECT_FIXTURE})
    endforeach()
    if (MISTRAL_CHECKPOINT_YOSYS)
        add_test(NAME nextpnr-${family}-placed-checkpoint
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/placed_checkpoint.py
                    --yosys ${MISTRAL_CHECKPOINT_YOSYS}
                    --nextpnr $<TARGET_FILE:nextpnr-${family}>
                    --output ${CMAKE_CURRENT_BINARY_DIR}/tests/placed-checkpoint)
        add_test(NAME nextpnr-${family}-physical-lut-timing
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/physical_lut_timing.py
                    --yosys ${MISTRAL_CHECKPOINT_YOSYS}
                    --nextpnr $<TARGET_FILE:nextpnr-${family}>
                    --output ${CMAKE_CURRENT_BINARY_DIR}/tests/physical-lut-timing)
        add_test(NAME nextpnr-${family}-physical-lut-timing-isolated
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/physical_lut_timing.py
                    --yosys ${MISTRAL_CHECKPOINT_YOSYS}
                    --nextpnr $<TARGET_FILE:nextpnr-${family}>
                    --output ${CMAKE_CURRENT_BINARY_DIR}/tests/physical-lut-timing-isolated
                    --isolated-lut-placement)
        # Real routes. pad-clock above only checks synthetic route strings.
        add_test(NAME nextpnr-${family}-pad-clock-route
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/pad_clock_route.py
                    --yosys ${MISTRAL_CHECKPOINT_YOSYS}
                    --nextpnr $<TARGET_FILE:nextpnr-${family}>
                    --output ${CMAKE_CURRENT_BINARY_DIR}/tests/pad-clock-route)
    endif()
    add_test(NAME nextpnr-${family}-capture-pipeline-locality-cli
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/capture_pipeline_locality_cli.py
                $<TARGET_FILE:nextpnr-${family}> -v)
    add_test(NAME nextpnr-${family}-lut-pair-placement-cli
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/lut_pair_placement_cli.py
                $<TARGET_FILE:nextpnr-${family}> -v)
    add_test(NAME nextpnr-${family}-timing-report-paths-cli
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/timing_report_paths_cli.py
                $<TARGET_FILE:nextpnr-${family}> -v)
    add_test(NAME nextpnr-${family}-lut-driver-copy-cli
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/lut_driver_copy_cli.py
                $<TARGET_FILE:nextpnr-${family}> -v)
    add_test(NAME nextpnr-${family}-decomposition-cli
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/decomposition_cli.py
                $<TARGET_FILE:nextpnr-${family}> -v)
    add_test(NAME nextpnr-${family}-comb-remap-plan-cli
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/comb_remap_plan_cli.py
                $<TARGET_FILE:nextpnr-${family}> -v)
    add_test(NAME nextpnr-${family}-remap-plan-cli
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/remap_plan_cli.py
                $<TARGET_FILE:nextpnr-${family}> -v)
    add_test(NAME nextpnr-${family}-arrival-pin-assignment-cli
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/arrival_pin_assignment_cli.py
                $<TARGET_FILE:nextpnr-${family}>)
    add_test(NAME nextpnr-${family}-local-remap-cli
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/local_remap_cli.py
                $<TARGET_FILE:nextpnr-${family}> -v)
    add_test(NAME nextpnr-${family}-reduction-balance-cli
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/reduction_balance_cli.py
                $<TARGET_FILE:nextpnr-${family}> -v)
endif()

set(MISTRAL_ROOT "" CACHE STRING "Mistral install path")
set(MISTRAL_DONT_INSTALL ON)

add_subdirectory(${MISTRAL_ROOT}/tools ${CMAKE_CURRENT_BINARY_DIR}/tools)
add_subdirectory(${MISTRAL_ROOT}/generator ${CMAKE_CURRENT_BINARY_DIR}/generator)
add_subdirectory(${MISTRAL_ROOT}/libmistral ${CMAKE_CURRENT_BINARY_DIR}/libmistral)

find_package(LibLZMA REQUIRED)

target_include_directories(nextpnr-${family}-core INTERFACE
    ${MISTRAL_ROOT}/libmistral
    ${CMAKE_CURRENT_BINARY_DIR}/tools
    ${CMAKE_CURRENT_BINARY_DIR}/libmistral
    ${LIBLZMA_INCLUDE_DIRS}
)

target_link_libraries(nextpnr-${family}-core INTERFACE
    mistral
    ${LIBLZMA_LIBRARIES}
)

if (BUILD_TESTS)
    add_executable(nextpnr-${family}-ff-core-config tests/ff-core/config.cc)
    target_include_directories(nextpnr-${family}-ff-core-config PRIVATE
        ${MISTRAL_ROOT}/libmistral ${CMAKE_CURRENT_BINARY_DIR}/tools
        ${CMAKE_CURRENT_BINARY_DIR}/libmistral ${LIBLZMA_INCLUDE_DIRS})
    target_link_libraries(nextpnr-${family}-ff-core-config PRIVATE mistral ${LIBLZMA_LIBRARIES})
    add_test(NAME nextpnr-${family}-ff-core-reference
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ff-core/test_reference.py
                --config-reader $<TARGET_FILE:nextpnr-${family}-ff-core-config>)
    add_test(NAME nextpnr-${family}-ff-core-native-reference
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ff-core/model_reference.py
                --check --config-reader $<TARGET_FILE:nextpnr-${family}-ff-core-config>)
    add_test(NAME nextpnr-${family}-ff-core-native-parser
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ff-core/test_model_reference.py)
    add_test(NAME nextpnr-${family}-ff-core-reference-cli
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ff-core/check_cli.py
                $<TARGET_FILE:nextpnr-${family}>)
endif()

# The Cyclone V coordinate constructors take narrow integer types, and the
# arch builds them from ints in braced initialisers. GCC warns about that
# narrowing; clang (as on macOS) makes it an error unless told to warn too.
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(nextpnr-${family}-core INTERFACE -Wno-error=c++11-narrowing)
endif()
