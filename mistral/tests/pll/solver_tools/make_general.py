#!/usr/bin/env python3
"""Build a Quartus-checked end-to-end PLL fixture for mistral/tests/pll/general.py.

Usage: make_general.py CASEDIR
CASEDIR contains top.v, pins.qsf and clocks.sdc.  nextpnr places the PLLs;
Quartus is then pinned to the same FPLL sites and output counters, compiled,
and its decoded FPLL settings are stored with the fixture.
"""
import gzip, hashlib, json, os, re, shutil, subprocess, sys
from pathlib import Path

Q = Path(os.environ.get("QUARTUS_BIN", "quartus/bin"))
YOSYS = Path(os.environ.get("YOSYS", "yosys"))
NEXTPNR = Path(os.environ.get("NEXTPNR", "nextpnr-mistral"))
CV = Path(os.environ.get("MISTRAL_CV", "mistral-cv"))
COUNTER_Y = {(0, 0): lambda k: k, (0, 14): lambda k: 14 + k, (0, 31): lambda k: 39 - k,
             (0, 55): lambda k: 55 + k, (0, 73): lambda k: 81 - k, (89, 0): lambda k: k}


def placements(log):
    found = {}
    for m in re.finditer(r"PLL '([^']+)': .* counters C([0-9,]+), bel altera_pll\.(\d+)\.(\d+)\.\d+", log):
        found[m.group(1)] = (int(m.group(3)), int(m.group(4)), [int(c) for c in m.group(2).split(",")])
    return found


def main():
    case = Path(sys.argv[1]).resolve()
    work = case / "build"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir()
    subprocess.run([str(YOSYS), "-q", "-p", f'read_verilog "{case / "top.v"}"; synth_intel_alm -nobram -nolutram '
                    f'-nodsp -top top; write_json "{work / "synth.json"}"'], check=True)
    log = subprocess.run([str(NEXTPNR), "--device", "5CSEBA6U23I7", "--qsf", str(case / "pins.qsf"),
                          "--sdc", str(case / "clocks.sdc"), "--json", str(work / "synth.json"),
                          "--rbf", str(work / "nextpnr.rbf")], capture_output=True, text=True)
    assert log.returncode == 0, log.stdout[-3000:] + log.stderr[-3000:]
    place = placements(log.stdout + log.stderr)
    assert place, "no PLL placement found"
    qsf = [
        'set_global_assignment -name FAMILY "Cyclone V"',
        "set_global_assignment -name DEVICE 5CSEBA6U23I7",
        "set_global_assignment -name TOP_LEVEL_ENTITY top",
        "set_global_assignment -name VERILOG_FILE top.v",
        "set_global_assignment -name SDC_FILE clocks.sdc",
        "set_global_assignment -name PROJECT_OUTPUT_DIRECTORY output_files",
        "set_global_assignment -name GENERATE_RBF_FILE ON",
        "set_global_assignment -name ON_CHIP_BITSTREAM_DECOMPRESSION OFF",
        'set_global_assignment -name STRATIXV_CONFIGURATION_SCHEME "PASSIVE SERIAL"',
        "set_global_assignment -name ENABLE_CONFIGURATION_PINS OFF",
        "set_global_assignment -name NUM_PARALLEL_PROCESSORS 2",
    ]
    for line in (case / "pins.qsf").read_text().splitlines():
        if line.strip():
            qsf.append(line)
            m = re.match(r"set_location_assignment PIN_\w+ -to (\S+)", line)
            if m:
                qsf.append('set_instance_assignment -name IO_STANDARD "3.3-V LVTTL" -to %s' % m.group(1))
    for name, (x, y, counters) in sorted(place.items()):
        qsf.append('set_instance_assignment -name PLL_COMPENSATION_MODE DIRECT -to "%s|*"' % name)
        qsf.append('set_location_assignment FRACTIONALPLL_X%d_Y%d_N0 -to "%s|general[0].gpll~FRACTIONAL_PLL"'
                   % (x, y + 1, name))
        for i, k in enumerate(counters):
            qsf.append('set_location_assignment PLLOUTPUTCOUNTER_X%d_Y%d_N1 -to "%s|general[%d].gpll~PLL_OUTPUT_COUNTER"'
                       % (x, COUNTER_Y[(x, y)](k), name, i))
    q = work / "quartus"
    q.mkdir()
    shutil.copy(case / "top.v", q / "top.v")
    shutil.copy(case / "clocks.sdc", q / "clocks.sdc")
    (q / "top.qsf").write_text("\n".join(qsf) + "\n")
    (q / "top.qpf").write_text('PROJECT_REVISION = "top"\n')
    res = subprocess.run([str(Q / "quartus_sh"), "--flow", "compile", "top"], cwd=q, capture_output=True, text=True)
    (work / "quartus.log").write_text(res.stdout + res.stderr)
    assert res.returncode == 0, "\n".join(l for l in res.stdout.splitlines() if l.startswith("Error"))[:3000]
    rbf = q / "output_files/top.rbf"
    subprocess.run([str(CV), "decomp", "5CSEBA6U23I7", str(rbf), str(work / "quartus.bt")], check=True,
                   capture_output=True)
    bt = (work / "quartus.bt").read_text()
    settings = [l for l in bt.splitlines() if re.match(r"^[si] (FPLL|CMUX)", l)]
    (case / "pll-settings.txt").write_text("\n".join(settings) + "\n")
    (case / "top.qsf").write_text("\n".join(qsf) + "\n")
    rpt = (q / "output_files/top.fit.rpt").read_text(encoding="latin-1")
    a = rpt.find("; PLL Usage Summary")
    start = rpt.rfind("\n", 0, rpt.rfind("\n", 0, a)) + 1
    (case / "fitter-pll.txt").write_text(rpt[start:rpt.find("\n\n", a)] + "\n")
    raw = rbf.read_bytes()
    with gzip.GzipFile(case / "top.rbf.gz", "wb", mtime=0) as f:
        f.write(raw)
    sha = {"top.rbf": hashlib.sha256(raw).hexdigest(),
           "top.rbf.gz": hashlib.sha256((case / "top.rbf.gz").read_bytes()).hexdigest()}
    (case / "sha256.txt").write_text("".join("%s  %s\n" % (v, k) for k, v in sha.items()))
    (case / "placement.txt").write_text("".join("%s %d %d %s\n" % (n, x, y, ",".join(map(str, c)))
                                                for n, (x, y, c) in sorted(place.items())))
    shutil.rmtree(work)
    print("OK", case.name, place)


if __name__ == "__main__":
    main()
