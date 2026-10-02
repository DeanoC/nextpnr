# Experimental capture pipeline locality

This diagnostic places two existing registers together when a registered
output from a fixed hard block feeds a plain capture register whose only
consumer is a second plain register. It changes placement only: net owners,
connections, register initial values and pipeline latency stay unchanged.
Discovery uses cell types, timing arcs and graph structure. It does not match
RAM-test instance names, pins or saved coordinates.

Enable it explicitly on a fresh ordinary HeAP run:

```sh
NEXTPNR_MISTRAL_CAPTURE_PIPELINE_LOCALITY="timing.json 8 24" nextpnr-mistral ...
```

The three fields are a current pre-route timing report, an attempted-chain
budget from 1 through 64 and a Manhattan search radius from 1 through 24.
Request bytes are loaded before packing and placement. Saved JSON settings
cannot activate the pass. Routed reports with newly inserted feed-through
buffers are not normalized; the complete relevant paths must match the
current native graph before any placement trial. Use the same input and
placement options that produced the guide.

The pass runs after local and internal-cut remaps and before placed reduction
and later remap stages. It requires successful ordinary HeAP placement, even
with `--force`. Packing or placement bypasses, processed input, slot placement,
an earlier local or internal-cut listing and the single-register capture
locality diagnostic cannot accompany it.

Both registers must be movable, use the same clock and edge, and have no
connected enable, reset, synchronous load or secondary-data controls. Shared
first-register outputs are excluded. Protected cells, protected LABs, carry
chains, MLABs and constrained regions are excluded. Each target consumes an
empty LUT/FF half suitable for a feed-through, with its partner FF empty.
The search keeps up to 32 distinct LABs per stage and tests at most 16 distinct
LAB pairs per attempted chain.

Every trial checks native physical legality in all affected LABs and complete
clock-pair timing at both registers and every reached registered endpoint.
The first register must gain at least 250 ps of setup margin. Only the internal
same-clock, same-edge full-cycle hop may consume positive setup margin; it must
remain passing. All other affected setup margins must not regress. Related
hold checks must remain passing, or not worsen an existing failure. Unrelated
clock pairs are also compared without clock skew. Global clock Fmax must not
regress, and the pass cannot introduce or worsen a hold failure.

The transaction preserves graph identities, raw consumer slots, ordered ports,
parameters, constraints and pin maps. Rejection or an exception restores both
register BELs, strengths and all saved architecture and LAB caches, including
previously accepted pairs.

Placement qualification is not routed timing acceptance. A retained pair still
needs a full route and signoff under the required clocks, plus independent
graph and physical checks before claiming a gain. This prototype has no
qualified RAM-test gain yet and does not change the default build recipe.
