# MLAB initialization

`MISTRAL_MLAB` represents one writable 32×1 memory lane. nextpnr accepts an
optional numeric `INIT` parameter of at most 32 bits: bit `a` is the initial
value at address `a`. Missing high bits, omitted INIT, and unspecified x/z bits
are filled with zero. String values and values wider than 32 bits are rejected.
An unused partner lane retains zero initialization.

RAM initialization reverses the 32 address bits in each ALM half, inverts the
stored bits, then applies the existing MLAB CRAM permutation. The retained
[Quartus oracle](oracle/README.md) independently checks all twenty bit columns
of a 32×20 memory, including both lanes of each ALM. No Mistral table change is
needed.

## Frontend boundary

The locked Yosys revision `13b43f8c85ec430a33ee55d058fb4c32b42b6910` has
`init 0` in `intel_alm/common/lutram_mlab.txt` and no INIT parameter in its
MLAB simulation cell. This nextpnr change establishes the backend contract;
it does not enable initialized RTL memory inference in that Yosys revision.
A separate Yosys change must enable the mapping and initialize its simulation
cell from INIT. No misteross RTL, toolchain lock, or integration pin changed.

## Host regression

Start with the synthesized misteross `040_mlab_ram` fixture. `init.py` supplies
INIT directly to its eight MLAB cells, preserving the HPS GP interface and
write/read path. It checks every initialization bit in the decompressed RBF
against the corresponding placed lane, including unused halves.

```sh
python3 mistral/tests/mlab/init.py \
  --nextpnr /path/to/nextpnr-mistral --mistral-cv /path/to/mistral-cv \
  --fixture /path/to/misteross/build/oss/040_mlab_ram/synth.json \
  --qsf /path/to/misteross/boards/de10nano/pins.qsf \
  --sdc /path/to/misteross/boards/de10nano/clocks.sdc \
  --output /path/to/results --negative
```

Repeat with `--pattern zero`, `--pattern ones --width 7`,
`--pattern omitted --width 1`, and `--pattern unknown`. The last variant mixes
known bits with x/z bits. `--negative` checks malformed parameter diagnostics.
The original backend fails the data-pattern mask check because INIT is discarded.

## Hardware diagnostic, 2026-09-07

The data-pattern RBF was loaded on the configured DE10-Nano kit under a
`nextpnr-mlab-init` lease. The baseline returned zero at address 0 instead of
0xA6. With this change, [probe.sh](probe.sh) passed all 32 initial byte reads,
then overwrote even addresses and verified both new values and retained odd
addresses. Read checks occur after writes have completed; no same-cycle
read-during-write behavior is claimed.

Run the probe on the designated target only while holding its kit.py lease.
It accesses HPS GP registers and does not program hardware. Stop used kit.py's
automatic development reboot recovery, and the kit was returned free.

- nextpnr base: `39f194e8f26db1700edc3acf759f341e1b9fd90d` (merged PR #30).
- Mistral: `78ba2a580ae2523403d4f4f91891a6b11d7b6aba`, unchanged.
- Device: `5CSEBA6U23I7`; eight MLAB cells in four ALMs; HPS GP used=1.
- M10K, DSP and PLL used=0.
- Clock: `storage.FPGA_CLK1_50`, constraint 50 MHz, achieved 343.879 MHz.
- Compressed RBF: 1,954,120 bytes.
- RBF SHA-256: `0762720891358a0acbb5a65b8922731e4a6fc58ac9c496a2f452395e44cfd2f8`.

```text
PASS: all 32 initialized bytes
PASS: writes update alternate addresses and preserve unwritten bytes
```
