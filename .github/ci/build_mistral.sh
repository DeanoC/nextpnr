#!/bin/bash

export MISTRAL_PATH=${DEPS_PATH}/mistral

function get_dependencies {
    # Fetch mistral
    mkdir -p ${MISTRAL_PATH}
    git clone --recursive https://github.com/DeanoC/mistral.git ${MISTRAL_PATH}
    pushd ${MISTRAL_PATH}
    git reset --hard ${MISTRAL_REVISION}
    popd
}

function build_nextpnr {
    mkdir build
    pushd build
    cmake .. -DARCH=mistral -DMISTRAL_ROOT=${MISTRAL_PATH} -DBUILD_TESTS=ON
    make nextpnr-mistral nextpnr-heap-control-set-test -j`nproc`
    popd
}

function run_tests {
    ctest --test-dir build -R '^nextpnr-heap-control-set-test$' --output-on-failure
    python3 mistral/tests/router2_undriven.py --nextpnr build/nextpnr-mistral --output build/router2-undriven
}

function run_archcheck {
    pushd build
    ./nextpnr-mistral --device 5CEBA2F17A7 --test
    popd
}
