# FES Spectrum single-wire GPU-router plateau qualification fixture

Corresponding source: https://github.com/DeanoC/fes/tree/deb0f18d53467083845c0d7a5f4b1d9b7876d4e4/sources/misteross
Original integration discussion: https://github.com/DeanoC/fes/pull/462

This fixture is reconstructed with authenticated Yosys e2d425de from the committed native-NMOS Spectrum shell and its original functional BUILD_ID b1ae20253692594f8a2676446fc49abd. The reconstruction's execution fingerprint exactly matches the captured original record (see provenance.json). It was **not rerouted**. The original route directory was removed by a later producer invocation, so the original full route logs and synthesis digest are unavailable. Captured log excerpts are included and labeled accordingly.

The original observation is on HIP nextpnr 0259c6dc, before the fix for https://github.com/DeanoC/nextpnr/issues/98 . This is a qualification case for that existing fix, not a claim that current nextpnr still fails.

## Decode and run

```sh
base64 --decode synth.json.gz.b64 | gzip --decompress > synth.json
sha256sum synth.json constraints.qsf clocks.sdc
nextpnr-mistral --json synth.json --device 5CSEBA6U23I7 \
  --qsf constraints.qsf --sdc clocks.sdc --freq 74.25 \
  --seed 2 --placer-heap-timingweight 2000 --placer-heap-critexp 5 \
  --report timing.json --write routed.json --rbf core.rbf --compress-rbf \
  --detailed-timing-report --timing-allow-fail --router gpu --gpu-device 0
```

Use the locked old stack for baseline, then repeat the same fixture/constraints with a maintained nextpnr build containing 53e1ad42. Preserve full logs; compare complete routing and zero overuse separately from final analogue clock timing. To investigate the second observation, change only `--seed 2` to `--seed 4`; the original producer stopped that attempt after 600 seconds while routing. No route was run during fixture reconstruction.

`--timing-allow-fail` is present for diagnostic reports. FES package sealing still independently requires 52.224 MHz system, 74.25 MHz pixel, 12.288 MHz audio and all ROM/socket checks; this fixture does not waive those gates.

## Contents and privacy

`synth.json.gz.b64` contains a compressed mapped netlist, not a firmware image. All sixteen firmware M10K INITs are zero. No Sinclair ROM, user content, device captures, credentials, or machine configuration files are included. Absolute source-attribution paths are replaced by `/repro/fes` and `/repro/toolchain`; cell names, parameters, ports and connectivity are unchanged. SHA256SUMS includes the decoded portable netlist digest.

## Source and licensing

The Spectrum shell and mailbox are GPL-2.0-or-later; the original FES Z80 engine is MIT (their SPDX notices and corresponding source are at the immutable source link above). COPYING provides GPLv2. Yosys's generated Intel ALM mapping primitives and their license notices are available at https://github.com/DeanoC/yosys/tree/e2d425dee148cc60c50f4e9b354a10d90eab15f4/techlibs/intel_alm . This diagnostic fixture preserves those source licenses and is supplied without warranty.
