# Native minimum routing scale

Quartus 17.0.2 applies a global minimum-routing scale after its per-driver
correlation factors. Mistral's current analogue circuit intervals include the
driver factors, but omit this additional stage. The retained native model-setup
observations on `5CSEBA6U23I7` are:

| PVT | Data minimum | Clock minimum | Additional maximum |
| --- | ---: | ---: | ---: |
| Slow 100 C | 0.921478271 | 0.965408325 | 1 |
| Slow -40 C | 0.919657171 | 0.963500321 | 1 |
| Fast 100 C | 0.930693090 | 0.975062370 | 1 |
| Fast -40 C | 0.930693090 | 0.975062370 | 1 |

These are observed native setup arguments, rather than fitted path ratios.
`native-scale-observer.cc` forwards every argument unchanged to the original
setter, then names the native PVT through the vendor's own model getter. Its
two object offsets and old string ABI are tied to the exact installed library
hashes in `reference.json`; this observer is a diagnostic for that build only.
It does not implement the compiler correction or change a timing factor.
The two `.log.gz` fixtures preserve the raw native output byte for byte.

The source reproduces the RAM/FF control design retained in FES
`175ec40040ac9e702595acf9a606d7ef4fec5358`. The compressed fitted RBF is
included so the FF-to-LUT routing control can run without Quartus. Its raw
digest and every retained input digest are checked by `generate.py --check`.
An independent FF-slot fit, exposing 100 C conditions in its timing database, returns
identical data/clock scale parameters at both slow and fast corners.
After copying `source/*` into an empty directory, compile with Quartus 17.0.2:

```sh
quartus_sh --flow compile top
c++ -O2 -D_GLIBCXX_USE_CXX11_ABI=0 -fPIC -shared \
  /path/to/native-scale-observer.cc -ldl -o observer.so
env LD_PRELOAD="$PWD/observer.so" quartus_sta -t /path/to/native-scale-observer.tcl
```

The separate default-off `--mistral-lut-route-min-reference` applies only the
slow100 **data** scale, to ordinary native routing elements feeding Boolean
LUT inputs. It requires the LUT core reference, routed delay ranges and the
characterized device. Clock routing elements and other devices reject.
The other PVT and clock observations remain reference data; this option does
not provide their routing corrections. The setting and reference digest bind
checkpoint acknowledgement and measured calibration identity.

At each completed hop the correction encloses the old interval and its
minimum-scaled early value, preserving negative advances conservatively. Scaling
a final sum containing positive and negative hops can be optimistic. Voltage
waveforms are propagated unchanged. The net and its separately charged final
LUT input buffer use the same corrected observations and outward ps rounding.
Default timing, routing, clocks and placement are unchanged when the option is
absent.

The factor explains much of the RAM/FF early discrepancy. It does not remove
every analogue-versus-Quartus residual, supply missing M10K/PLL waveforms, or
qualify a complete source/route/cell model. Missing observations still reject.
No whole-board timing, production seal or hardware acceptance follows from
these parameter observations or host tests.
