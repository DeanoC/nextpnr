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
    delay.cc
    local_remap.cc
    reduction_balance.cc
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
    TEST_SOURCES tests/completed_observations.cc tests/fes_cram.cc tests/enable_replication/backend.cc tests/placed_timeout.cc tests/local_remap.cc
    MAIN_SOURCE  main.cc
)

if (BUILD_TESTS)
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
