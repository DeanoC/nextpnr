#!/bin/bash
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PRIOR=/home/deano/harnesses/pcxt-dynamic-ce-coverage
NEXTPNR_TEST_IO_CHECKPOINT=/home/deano/harnesses/pcxt-endpoint-coverage/paired/inputs/placed.json \
NEXTPNR_TEST_IO_SDC="$PRIOR/synthetic-io.sdc" \
NEXTPNR_TEST_REGISTERED_OVERLAY="$PRIOR/reference-profile-overlay.json" \
NEXTPNR_TEST_REGISTERED_SDC="$PRIOR/registered-synthetic.sdc" \
NEXTPNR_TEST_FORWARD_OVERLAY="$ROOT/forward-overlay.json" \
NEXTPNR_TEST_RESET_OVERLAY="$ROOT/reset-overlay.json" \
NEXTPNR_TEST_IO_DIAGNOSTIC="${1:?new output path required}" \
 "$ROOT/final-bin/nextpnr-mistral-test" --gtest_filter=ExternalEndpointDiagnostic.PlacedCheckpointSyntheticInputConstraints
