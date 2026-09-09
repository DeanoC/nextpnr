# Mixed-width M10K simple dual-port RAM

The paired Yosys `intel_alm` mapping opts in with `ram_style="m10k_mixed"`.
It emits one `MISTRAL_M10K` with independent write and read geometries:
1024x10, 512x20 or 256x40. Eight-bit payload lanes use padded 10-bit physical
lanes, so 8/16/32-bit payload widths have the same lane ordering.

`CFG_MIXED_WIDTH=1` selects this path. `CFG_ABITS`/`CFG_DBITS` describe the
write port; `CFG_RD_ABITS`/`CFG_RD_DBITS` describe the read port. Both clocks
are required and `CFG_DUAL_CLOCK=1` must be set. Write enable is `A1EN`, read
address/core enable is `B1EN`. Both clocks use rising edges. INIT retains the
existing 1024x10 layout: the lowest-address lane is the least-significant
slice of a wide word.

See [mixed_width.v](mixed_width.v) for inferable Verilog. Explicit address
concatenation in the generated lanes lets Yosys share them into wide ports.
Arithmetic address expressions can be lowered to logic before port sharing
and fail to infer a single block.

Run the host sweep with the paired Yosys and nextpnr builds:

```sh
python3 mistral/tests/m10k/mixed_width.py \
  --yosys /path/to/yosys --nextpnr /path/to/nextpnr-mistral \
  --mistral-cv /path/to/mistral-cv \
  --qsf /path/to/misteross/boards/de10nano/pins.qsf \
  --sdc /path/to/misteross/boards/de10nano/clocks.sdc \
  --output /tmp/m10k-mixed-width
```

The eleven-case sweep covers 40↔10, 20↔10, 40↔20, tagged 10/20/40-bit
equal-width ports, and padded 32↔8 payloads. It checks
inference, both port geometries, one M10K/PLL/HPS GP, compressed RBF output,
clock routing, decoded settings and 50 MHz write-clock timing, plus six
invalid-configuration diagnostics. The read clock is a gated 25 MHz PLL
output from the same 50 MHz reference; these settled-data probes do not
establish cross-domain timing acceptance. It uses the default router2 with a
120-second timeout per routing run
(adjustable with `--route-timeout`). The former two-wire stall is covered by
[the retained congestion regression](../router2/README.md); Mistral now allows
router2 to expand the search box when congestion persists. The earlier
hardware acceptance artifacts retain their original router1 provenance.

The target probe is generated separately, for example:

```sh
python3 mistral/tests/m10k/mixed_width_probe.py \
  --unit 10 --write-lanes 4 --read-lanes 1 > /tmp/probe.sh
```

Run that script only on the designated kit under a `kit.py` session after
loading the corresponding RBF. The probe checks initialized data, wide-word
slice ordering, narrow writes preserving neighbors, writes with the read
clock stopped, and read-enable hold. Writes stage address/data before WE and
remove WE before changing address because the HPS GP bus is asynchronous.
Use the normal kit stop/reboot recovery and release protocol afterward.

Mixed-width byte enables and true dual-port writes are unsupported. There
is no defined result for a simultaneous read/write collision on overlapping
storage. Existing untagged inference and legacy M10K modes retain their
mapping. No Mistral database changes are required.

Quartus configuration references, including the actual compressed oracle
artifacts, are in [oracle/mixed-width](oracle/mixed-width). The Yosys companion
contains bounded mapped-versus-RTL SAT checks; host routing alone does not
prove memory behavior.
