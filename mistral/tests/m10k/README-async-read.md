# Cyclone V M10K asynchronous read contract

Cyclone V M10K registers its read address. Selecting an unregistered output
register does not make the memory asynchronous. The [Cyclone V embedded memory
features](https://docs.altera.com/r/docs/683375/current/cyclone-v-device-handbook-volume-1-device-interfaces-and-integration/embedded-memory-features)
list asynchronous memory for MLAB flow-through reads and not for M10K.

[FES issue #260](https://github.com/DeanoC/fes/issues/260) exposed the false
contract: an inferred 256x40 M10K returned the previous address's word at
74.25 MHz, while held-address reads passed. With its clock stopped, changing
the read address left the data unchanged. The former 1.5 ns address-to-data
arc was an estimate and could report passing timing for an impossible mode.
Static readback of 10-bit and 20-bit instances did not establish an
asynchronous read; the hardware specification rules out those widths too.

The packer rejects `CFG_ASYNC_READ=1` for simple and true dual-port M10K at
all widths. It also rejects the legacy simple-dual representation with an
omitted `B1EN`. Use MLAB, logic, or a registered M10K read with the required
pipeline alignment. `unsupported_async.py` tests all three fixed
simple-dual geometries and true-dual mode, and confirms the matching
synchronous variants still pack:

```sh
python3 mistral/tests/m10k/unsupported_async.py \
  --yosys /path/to/yosys --nextpnr /path/to/nextpnr-mistral \
  --qsf /path/to/pins.qsf --sdc /path/to/clocks.sdc \
  --output /tmp/m10k-async-reject
```
