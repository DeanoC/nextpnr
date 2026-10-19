#!/bin/bash


export XRAY_DB_PATH=${DEPS_PATH}/prjxray-db
export PEPPERCORN_PATH=${DEPS_PATH}/prjpeppercorn


function get_dependencies {
    # Fetch prjxray-db
    git clone https://github.com/openXC7/prjxray-db ${XRAY_DB_PATH}
    # Fetch apycula
    pip install --break-system-packages apycula
    # Fetch prjpeppercorn. v1.14 (783fe9db, 2026-10-19) breaks the gatemate
    # chipdb; stay on the parent commit until that bump is adopted on purpose.
    git clone https://github.com/YosysHQ/prjpeppercorn ${PEPPERCORN_PATH}
    git -C ${PEPPERCORN_PATH} checkout --detach b1eb52f45cac54a86e63401d02b1eb48c7d7b923
}

function build_nextpnr {
    mkdir build
    pushd build
    cmake .. -DARCH=himbaechel -DHIMBAECHEL_UARCH="gowin;xilinx;example;gatemate" -DHIMBAECHEL_EXAMPLE_DEVICES=example \
        -D HIMBAECHEL_XILINX_DEVICES="xc7a50t" -D HIMBAECHEL_PRJXRAY_DB=${XRAY_DB_PATH} \
        -D HIMBAECHEL_GOWIN_DEVICES="GW1N-9C;GW5A-25A" \
        -D HIMBAECHEL_PEPPERCORN_PATH=${PEPPERCORN_PATH}
    make nextpnr-himbaechel -j`nproc`
    popd
}

function run_tests {
    :
}

function run_archcheck {
    pushd build
    ./nextpnr-himbaechel --device EXAMPLE --test
    popd
}
