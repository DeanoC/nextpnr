# Retained FES Spectrum routing failure, 792b24805 GPU 0

These are retained original producer inputs and **complete** seed 4/seed 5 route logs. Source commit: https://github.com/DeanoC/fes/tree/792b24805ce714e890485c71fe122ebf0e445c8e/sources/misteross . Original BUILD_ID: e5efe9634129e80ad5c4a8429a897fea. No reconstruction or route was performed while packaging this fixture.

Seed 4 / weight 2000 / critexp 5 returned 125 after a single-wire plateau at iteration 104: TD.10.44.20, occ=2, reserved=-1, gp_mailbox.unit0_size[11] and [7]. Seed 5 completed routing; final analogue timing is recorded separately. Tools are unchanged from https://github.com/DeanoC/nextpnr/issues/114 : nextpnr 0259c6dc, Mistral 7ed06e21, Yosys e2d425de, HIP GPU 0 on Cyclone V 5CSEBA6U23I7. Full tool IDs and input hashes are included.

## Decode and replay

```sh
base64 --decode synth.json.gz.b64 | gzip --decompress > synth.json
sha256sum --check SHA256SUMS
nextpnr-mistral --json synth.json --device 5CSEBA6U23I7 \
  --qsf socket.qsf --sdc clocks.sdc --freq 74.25 \
  --seed 4 --placer-heap-timingweight 2000 --placer-heap-critexp 5 \
  --report timing.json --write routed.json --rbf core.rbf --compress-rbf \
  --detailed-timing-report --timing-allow-fail --router gpu --gpu-device 0
```

All sixteen firmware INITs are zero. No Sinclair ROM, user content/captures, credentials or machine configuration is included. Absolute source-attribution and tool paths are replaced by /repro/fes and /repro/toolchain, including path-derived names in unused blackbox simulation modules. All TOP mapped primitive names, parameters, ports and connectivity are unchanged. SHA256SUMS identifies portable files; provenance.json also gives original unsanitized file digests. Logs are complete and path sanitization is explicit.

## Licensing and qualification limits

Spectrum shell/mailbox GPL-2.0-or-later, original FES Z80 MIT; corresponding source and SPDX notices are at the immutable source link above. COPYING provides GPLv2. Generated Intel ALM mapping primitives and notices: https://github.com/DeanoC/yosys/tree/e2d425dee148cc60c50f4e9b354a10d90eab15f4/techlibs/intel_alm . Preserve source licenses.

This is another candidate for the already fixed legacy shortcut from https://github.com/DeanoC/nextpnr/issues/98 , not a confirmed latest-head defect. The ready fixed nextpnr 655f3833 packing also rejects this shell's CFG_ASYNC_READ=1 raw firmware M10K lanes; treat that as a separate compatibility result and preserve this fixture when qualifying the router fix. Check completed routing/zero overuse separately from final analogue timing. FES still requires 52.224/74.25/12.288 MHz and all ROM/socket seal checks; --timing-allow-fail is diagnostic only.
