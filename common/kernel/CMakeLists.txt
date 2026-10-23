add_library(nextpnr_kernel INTERFACE)

target_include_directories(nextpnr_kernel INTERFACE .)

target_sources(nextpnr_kernel PUBLIC
    arch_api.h
    archcheck.cc
    arch_pybindings_shared.h
    array2d.h
    base_arch.h
    base_clusterinfo.h
    basectx.cc
    basectx.h
    chain_utils.h
    command.cc
    command.h
    command_settings.h
    context.cc
    context.h
    design_utils.cc
    design_utils.h
    deterministic_rng.h
    embed.cc
    embed.h
    handle_error.cc
    hashlib.h
    idstring.cc
    idstring.h
    idstringlist.cc
    idstringlist.h
    indexed_store.h
    io_delay.h
    log.cc
    log.h
    nextpnr_assertions.cc
    nextpnr_assertions.h
    nextpnr_base_types.h
    nextpnr.cc
    nextpnr.h
    nextpnr_namespaces.cc
    nextpnr_namespaces.h
    nextpnr_types.cc
    nextpnr_types.h
    property.cc
    property.h
    pybindings.cc
    pybindings.h
    pycontainers.h
    pywrappers.h
    relptr.h
    report.cc
    sdc.cc
    sdf.cc
    sso_array.h
    str_ring_buffer.cc
    str_ring_buffer.h
    svg.cc
    timing.cc
    timing.h
    timing_log.cc
    util.h
)

if (BUILD_TESTS)
    add_executable(nextpnr-command-settings-test tests/command_settings.cc)
    target_include_directories(nextpnr-command-settings-test PRIVATE
        ${CMAKE_SOURCE_DIR}/3rdparty/googletest/googletest/include
        .
    )
    target_link_libraries(nextpnr-command-settings-test PRIVATE gtest_main Boost::headers)
    set_property(TARGET nextpnr-command-settings-test PROPERTY RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    add_test(NAME nextpnr-command-settings-test COMMAND nextpnr-command-settings-test)
endif()

target_link_libraries(nextpnr_kernel INTERFACE
    nextpnr_version
    nextpnr_frontend
    nextpnr_json
    nextpnr_rust
    json11
)

if (BUILD_PYTHON)
    target_include_directories(nextpnr_kernel INTERFACE
        ${Python3_INCLUDE_DIRS}
    )

    target_link_libraries(nextpnr_kernel INTERFACE
        pybind11::headers
    )
endif()
