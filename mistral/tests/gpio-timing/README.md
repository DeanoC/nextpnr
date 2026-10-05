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
clock enables, and registered bidirectional output/OE data and enables on the
tested device with default delay settings. Both DDR outputs launch into the
fabric on the rising clock edge; the low word's falling pad-capture edge is a
separate relationship. Asynchronous clear stays uncharacterized. Other devices,
LVCMOS, bus hold, and explicit delay-chain settings retain their unsupported
classification.

Output-only `MISTRAL_SDROUT` remains excluded because its backend delay-chain
defaults differ from Quartus's fitted values. The oracle mapping explicitly
lists this difference. Copying Quartus's register arcs without first matching
those settings would not establish a timing model for the emitted bitstream.

External pad capture and output timing remain unsupported. The current IO-delay
analyzer refuses registered pads rather than silently timing only their fabric
side. Completing that support requires independent read/write boundaries on
bidirectional pads, separate capture edges and DDR handoff coverage, matching
output delay-chain settings, and a qualified clock-to-pin/output-load model.
Some internal DDR handoff queries report no paths; that absence is not proof of
zero delay or a passing half-cycle check.
