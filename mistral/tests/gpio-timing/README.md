# GPIO register timing characterization

`characterize.py` compiles the existing SDR/DDR/OE register fixtures in Quartus
17.0.2 Lite for 5CSEBA6U23I7 and 3.3-V LVTTL. It retains their RTL, QSF, SDC,
fitted databases, complete setup/hold paths, and hashes. TimeQuest queries all
four available slow/fast temperature corners. These are reference fits with
synthetic external constraints, not SDRAM constraints or hardware signoff.

```sh
python3 mistral/tests/gpio-timing/characterize.py \
  --quartus-bin /path/to/quartus/bin --output /tmp/gpio-timing
python3 mistral/tests/gpio-timing/summarize.py /tmp/gpio-timing/evidence.json \
  --output /tmp/gpio-timing/reference.json
```

`--reuse-fits` queries existing fits only after checking their source and
assignments; Quartus's `LAST_QUARTUS_VERSION` metadata and blank lines are
ignored when checking QSF identity. Changing an assignment still fails.

The adjacent reference receipt records observed maxima, their paths and input
hashes. Local register setup is the data CELL delay less the internal clock
CELL delay, plus intrinsic setup. Hold reverses the subtraction. Input
clock-to-fabric combines the internal register clock CELL and output CELL
paths. Clock-buffer, interconnect, package and output-buffer delays are excluded
from these local arcs. The backend uses maxima rounded outward to 10 ps and a
conservative zero clock-to-Q minimum. These reference envelopes do not certify
all silicon, placements, electrical conditions, or fast-corner routing delay.

The current backend profile covers fabric-facing SDR/DDR input outputs, input
clock enables, and registered SDR output/OE data and enables on the
tested device with default delay settings. Both DDR outputs launch into the
fabric on the rising clock edge; the low word's falling pad-capture edge is a
separate relationship. Asynchronous clear stays uncharacterized. Other devices,
LVCMOS, bus hold, and explicit delay-chain settings retain their unsupported
classification.

Output-only `MISTRAL_SDROUT` now keeps the database's zero delay-chain defaults,
matching these Quartus fits. Explicit `D5_DELAY` assignments still override
the default and retain their unsupported fabric timing classification.
`compare_sdrout.py` audits a second fit of `pads.v` with `D5_DELAY 31` on p4/p6.
Its QSF also requests `D5_OE_DELAY 31`; Quartus reports that the OE delay is
unused on those output-only pins and resets it to zero. The audit checks source
and SDC identity, permitted QSF differences, decoded data-delay selectors,
equal reported clock paths, and matched setup/hold paths at all four corners.

```sh
python3 mistral/tests/gpio-timing/compare_sdrout.py \
  --baseline /tmp/gpio-timing/pads --delayed /tmp/pads-forced31 \
  --output /tmp/sdrout-delay-comparison.json
```

The adjacent `sdrout-delay-reference.json` receipt records sixteen matched
clock-to-pin observations and 24 unchanged fabric setup/hold checks. Forced
D5=31 adds 0.483–1.180 ns in those fits. Removing that delay improves setup
but also reduces output hold margin; board timing must determine any required
explicit compensation. This is not evidence of the cause of issue #135, or
proof that either setting passes SDRAM hardware.

External pad capture and output timing remain unsupported. The current IO-delay
analyzer refuses registered pads rather than silently timing only their fabric
side. The common analyzer now supports independent read/write boundaries,
separate capture edges and data/OE channels through an architecture API, with
synthetic-model regression coverage. Mistral does not supply that API's pad
models yet. Completing its support requires DDR handoff coverage, matching
output delay-chain settings, and a qualified clock-to-pin/output-load model.
Some internal DDR handoff queries report no paths; that absence is not proof of
zero delay or a passing half-cycle check.
