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
    export PATH=${GITHUB_WORKSPACE}/.yosys/bin:$PATH
    mkdir build
    pushd build
    cmake .. -DARCH=mistral -DMISTRAL_ROOT=${MISTRAL_PATH} -DBUILD_TESTS=ON
    make nextpnr-mistral nextpnr-mistral-test nextpnr-mistral-gpio-core-config nextpnr-mistral-lut-carry-decode nextpnr-command-settings-test nextpnr-heap-control-set-test nextpnr-reserved-route-slack-test nextpnr-gpu-lease-test -j`nproc`
    popd
}

function run_tests {
    export PATH=${GITHUB_WORKSPACE}/.yosys/bin:$PATH
    python3 common/route/tests/router2_profile.py
    ctest --test-dir build -R '^nextpnr-(command-settings|heap-control-set|reserved-route-slack|gpu-lease)-test$' --output-on-failure
    ctest --test-dir build -R '^nextpnr-mistral-(command-settings|gpu-lease|routed-delay-ranges)-cli$' --output-on-failure
    ./build/nextpnr-mistral-test --gtest_filter='HpsInterrupts.*:HpsClocksResets.*:HpsLwBridge.*'
    ./build/nextpnr-mistral-test --gtest_filter='GpioCoreTimingTest.*'
    ./build/nextpnr-mistral-test --gtest_filter='LutCoreTimingTest.*:CriticalCohort.MeasuredModelBindsLutReferenceModeAndData'
    ./build/nextpnr-mistral-test --gtest_filter='LutRouteMinimum.*:LutRouteMinimumTest.*:CriticalCohort.MeasuredModelBindsLutRouteMinimumModeAndData'
    ./build/nextpnr-mistral-test --gtest_filter='IoDelayTest.RoutedRangesDoNotDependOnExternalIoConstraints:IoDelayTest.RoutedRangesAffectSkewAwareClockRoutes:CriticalCohort.MeasuredModelPreservesRoutedRangesAndRejectsDifferentRangeMode'
    ctest --test-dir build -R '^nextpnr-mistral-gpio-core-(reference-cli|parser|reference)$' --output-on-failure
    ctest --test-dir build -R '^nextpnr-mistral-lut-carry-(reference|parser)$' --output-on-failure
    ctest --test-dir build -R '^nextpnr-mistral-lut-core-(reference|cli)$' --output-on-failure
    ctest --test-dir build -R '^nextpnr-mistral-lut-route-min-(reference|cli)$' --output-on-failure
    python3 mistral/tests/router2_profile.py --nextpnr build/nextpnr-mistral --output build/router2-profile
    python3 mistral/tests/router2_undriven.py --nextpnr build/nextpnr-mistral --output build/router2-undriven
    python3 mistral/tests/pad_clock.py
    python3 mistral/tests/pad_clock_route.py \
        --yosys "$(command -v yosys)" \
        --nextpnr build/nextpnr-mistral \
        --output build/pad-clock-route
}

function run_archcheck {
    pushd build
    ./nextpnr-mistral --device 5CEBA2F17A7 --test
    popd
}
