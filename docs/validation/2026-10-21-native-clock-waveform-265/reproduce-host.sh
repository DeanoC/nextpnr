#!/bin/bash
set -eu
: "${NEXTPNR_TEST_BINARY:?set the freshly built test binary}"
: "${NEXTPNR_WAVEFORM_ROOT:?set this evidence/output directory}"
NEXTPNR_TEST_IO_CHECKPOINT=/home/deano/harnesses/pcxt-endpoint-coverage/paired/inputs/placed.json \
NEXTPNR_TEST_IO_SDC=/home/deano/harnesses/pcxt-dynamic-ce-coverage/synthetic-io.sdc \
NEXTPNR_TEST_REGISTERED_OVERLAY=/home/deano/harnesses/pcxt-dynamic-ce-coverage/reference-profile-overlay.json \
NEXTPNR_TEST_REGISTERED_SDC=/home/deano/harnesses/pcxt-dynamic-ce-coverage/registered-synthetic.sdc \
NEXTPNR_TEST_FORWARD_OVERLAY="$NEXTPNR_WAVEFORM_ROOT/forward-overlay.json" \
NEXTPNR_TEST_RESET_OVERLAY=/home/deano/harnesses/pcxt-interface-contracts/reset-overlay.json \
NEXTPNR_TEST_UNREGISTERED_OVERLAY=/home/deano/harnesses/pcxt-pad-models/dq-overlay.json \
NEXTPNR_TEST_IO_DIAGNOSTIC="${1:?new output path}" \
 "$NEXTPNR_TEST_BINARY" --gtest_filter=ExternalEndpointDiagnostic.PlacedCheckpointSyntheticInputConstraints
