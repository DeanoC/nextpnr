# Cyclone V MLAB initialization oracle

This writable 32 x 20-bit memory was compiled with Quartus Prime Lite
17.0.2 Build 602 for `5CSEBA6U23I7`. It occupies one MLAB (640 bits),
including both 32-bit memory lanes in each of its ten ALMs.
This is host-only configuration evidence; it has not been programmed.

The source uses `ramstyle = "MLAB, no_rw_check"` to request MLAB implementation
without promising read-during-write behavior. Initialization word at address
`a` is `((a * 0x18473) ^ (a*a * 0x2861) ^ 0xa65b9) & 0xfffff`.
All 20 source bit columns have distinct initialization patterns.

The bundled `top.rbf.gz` is a gzip-compressed copy of the original Quartus RBF.
Its uncompressed SHA256 is recorded in `provenance.json`. This compression is
an artifact-storage format, not FPGA configuration compression.

To check the retained oracle with an installed Mistral tool:

```sh
gzip -dc top.rbf.gz > top.rbf
mistral-cv decomp 5CSEBA6U23I7 top.rbf top.bt
python3 oracle_check.py
```

The checker derives each logical INIT column from `top.v` and compares it with
the physical MLAB `LUT_MASK` halves decoded from the retained RBF. It requires
all 20 columns to match. `init-mapping.json` records source bits, masks and
physical ALM/lane placements. The resulting mapping is:

```
LUT_MASK[p[31-address] + 32*lane] = !INIT[address]
p = [0,1,4,5,8,9,12,13,29,28,25,24,21,20,17,16,
     2,3,6,7,10,11,14,15,31,30,27,26,23,22,19,18]
```

Thus reverse the 32 logical address bits, invert their values, then apply
nextpnr's existing MLAB physical bit permutation. The bottom lane uses the same
permutation offset by 32. An unoccupied or zero-initialized lane is all ones.

To regenerate a separate Quartus oracle, use `quartus_sh --flow compile top`.
The generated bitstream need not match byte-for-byte if fitter placement changes;
run the same mask checker against its decomposed `output_files/top.rbf`.
