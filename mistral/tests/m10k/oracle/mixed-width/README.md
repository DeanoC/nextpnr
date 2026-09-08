# Quartus mixed-width configuration oracles

These standalone `altsyncram` projects were compiled with Quartus Prime Lite
17.0.2 Build 602 for `5CSEBA6U23I7`. `mapping.json` records hashes of the
uncompressed RBF files and decoded nondefault M10K settings. The RBFs are
retained here as gzip files so review does not depend on a private build path.
These are host configuration references, not the hardware acceptance images.

Rebuild a case in a writable copy using `quartus_sh --flow compile top`.
To inspect the retained artifact:

```sh
gzip -dc w20r40/top.rbf.gz > /tmp/w20r40.rbf
mistral-cv decomp 5CSEBA6U23I7 /tmp/w20r40.rbf /tmp/w20r40.bt
```

Address pins begin at physical bit `12 - address_width`, independently for
A and B. A 10-bit write duplicates its data onto DATAAIN[9:0] and [19:10];
a 20-bit write uses DATAAIN[19:0]; a 40-bit write adds DATABIN[19:0] as the
upper half. A 10/20-bit read uses DATABOUT; a 40-bit read uses DATAAOUT for
the lower half and DATABOUT for the upper half.

Write enable drives WREN.0 and ENABLE.1. Read enable drives ENABLE.0.
CLKIN.0 is the write clock and CLKIN.1 the read clock. Both physical data
widths are configured independently. Mixed-width modes involving 40 bits
clear both DATA_FLOW_THRU settings and BOT_1_INCLK_SEL; 10↔20 sets them.
FAST_WRITE is FAST. Missing settings in the decoder are defaults, not proof
that a field does not exist. No routing or Mistral table additions are needed.
