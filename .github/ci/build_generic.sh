#!/bin/bash

function get_dependencies {
    :
}

function build_nextpnr {
    mkdir build
    pushd build
    cmake .. -DARCH=generic -DWERROR=on -DBUILD_TESTS=ON
    make nextpnr-generic -j`nproc`
    popd
}

function run_tests {
    export PATH=${GITHUB_WORKSPACE}/.yosys/bin:$PATH
    ( export NPNR=$(pwd)/build/nextpnr-generic && cd tests/generic/flow && ./run.sh )
    ctest --test-dir build -R '^nextpnr-generic-command-settings-cli$' --output-on-failure
}

function run_archcheck {
    pushd build
    # TODO
    ./nextpnr-generic --uarch example --test
    popd
}
