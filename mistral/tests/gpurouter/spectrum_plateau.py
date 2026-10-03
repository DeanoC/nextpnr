#!/usr/bin/env python3
"""Qualify retained Spectrum routing independently of analogue timing closure.

Each invocation uses fresh output directories and preserves logs and summary
evidence, including unsuccessful runs. The default gate requires a complete
HIP route; --expect-failure explicitly checks an old initial-routing plateau
or the current packer's asynchronous-M10K rejection instead.
"""

import argparse
import gzip
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import time


FIXTURES = Path(__file__).with_name("spectrum_issue114")
CLOCKS = {
    "machine.ram.pixel_clk": 74.25,
    "system_clock.faithful.clocks[0]": 52.224,
    "system_clock.faithful.clocks[1]": 12.288,
}
ITERATION = re.compile(r"iter=(\d+) wires=(\d+) overused=(\d+) overuse=(\d+)\b")
BACKEND = re.compile(r"\bbackend ([^\n]+?) ready in [0-9.]+s")
CHECKSUM = re.compile(r"Info: Checksum: (0x[0-9a-f]+)")
TABLE_FMAX = re.compile(r"Routed Fmax \(pip delay table\) for clock '([^']+)': ([0-9.]+) MHz")
PLATEAU = re.compile(r"congestion plateau at 1 overused wires did not clear \(unfroze 0, 1 remain\)")
PACKING = re.compile(r"ERROR: M10K '[^\n]+': Cyclone V M10K does not support asynchronous reads;")


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_fixture(directory):
    """Authenticate the exact input files before creating any run outputs."""
    manifest = json.loads((directory / "fixture.json").read_text())
    qsf = manifest["qsf"]
    if Path(qsf).name != qsf or not qsf.endswith(".qsf"):
        raise ValueError("fixture qsf must name one .qsf file")
    seed = manifest["default_seed"]
    if not isinstance(seed, int) or isinstance(seed, bool) or seed < 1:
        raise ValueError("fixture default_seed must be positive")
    hashes = {name: sha256(directory / name) for name in ("synth.json.gz", qsf, "clocks.sdc")}
    synth = gzip.decompress((directory / "synth.json.gz").read_bytes())
    hashes["synth.json"] = hashlib.sha256(synth).hexdigest()
    for name, actual in hashes.items():
        if manifest["sha256"].get(name) != actual:
            raise ValueError(f"fixture SHA-256 mismatch for {name}")
    return manifest, hashes, synth


def log_evidence(text, returncode, timed_out=False):
    """Keep partial negotiation evidence separate from successful completion."""
    iterations = [dict(zip(("iteration", "wires", "overused", "overuse"), map(int, m)))
                  for m in ITERATION.findall(text)]
    backends = BACKEND.findall(text)
    checksums = CHECKSUM.findall(text)
    timeout = timed_out or re.search(r"timed out after [0-9.]+ seconds", text) is not None
    reached_zero = any(i["overused"] == 0 and i["overuse"] == 0 for i in iterations)
    last = iterations[-1] if iterations else None
    complete = "Routing complete." in text
    # An expected old-policy failure must be initial negotiation, with the
    # single unreserved shared wire and the exact unsuccessful escape marker.
    plateau = (returncode == 125 and not reached_zero and not complete
               and last is not None and last["overused"] == 1 and last["overuse"] == 1
               and PLATEAU.search(text) is not None
               and re.search(r"stalled: wire \S+ occ=2 reserved=-1 used by ", text) is not None
               and "GPU router did not converge (1 overused wires)." in text
               and "ERROR: Routing design failed." in text)
    packing = (returncode == 125 and not iterations and not complete
               and PACKING.search(text) is not None)
    outcome = ("timeout" if timeout else "packing_rejected" if packing else
               "initial_plateau" if plateau else "tool_failed" if returncode != 0 else "incomplete")
    return {
        "returncode": returncode, "timed_out": bool(timeout), "outcome": outcome,
        "backend": backends[-1] if backends else None,
        "last_iteration": last, "reached_zero_overuse": reached_zero,
        "route_complete": complete, "tool_complete": "Program finished normally." in text,
        "signoff": "Running signoff timing analysis..." in text,
        # Analogue candidate repair may change routing after router1 logs its
        # checksum. The final RBF hash below authenticates the emitted result.
        "last_logged_route_checksum": checksums[-1] if checksums else None,
        "table_fmax": {clock: float(value) for clock, value in TABLE_FMAX.findall(text)},
        "tool_errors": [line for line in text.splitlines() if line.startswith("ERROR:")],
    }


def timing_evidence(path):
    errors = []
    clocks = {}
    try:
        report = json.loads(path.read_text())
        raw_clocks = report.get("fmax", {})
        if not isinstance(raw_clocks, dict) or not raw_clocks:
            raise ValueError("timing report names no clocks")
        for name, raw in raw_clocks.items():
            achieved, constraint = raw["achieved"], raw["constraint"]
            if any(isinstance(n, bool) or not isinstance(n, (float, int))
                   or not math.isfinite(n) or n <= 0 for n in (achieved, constraint)):
                errors.append(f"clock {name}: achieved/constraint must be finite and positive")
                continue
            clocks[name] = {"achieved": achieved, "constraint": constraint,
                            "meets_constraint": achieved >= constraint}
        for name, target in CLOCKS.items():
            if name not in clocks:
                errors.append(f"expected clock missing or invalid: {name}")
            # Mistral's getDelayFromNS() truncates the clock period to whole
            # picoseconds (arch.h); timing.cc converts that period back into
            # reported MHz. Compare periods so one quantization step is valid
            # at every expected frequency, plus tiny float-report rounding.
            elif abs(1e6 / clocks[name]["constraint"] - 1e6 / target) > 1.01:
                errors.append(f"clock {name}: constraint period differs from {target} MHz by more than 1.01 ps")
        gates = {name: {"required": target, "achieved": clocks[name]["achieved"],
                        "passes": clocks[name]["achieved"] >= target}
                 for name, target in CLOCKS.items() if name in clocks}
        valid = not errors
        return {"fmax": clocks, "gates": gates,
                "signoff_pass": all(c["meets_constraint"] for c in clocks.values()) if valid else None,
                "fes_timing_pass": all(g["passes"] for g in gates.values()) if valid else None}, errors
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as exc:
        return {"fmax": {}, "gates": {}, "signoff_pass": None, "fes_timing_pass": None}, [f"invalid timing report: {exc}"]


def run(args, manifest, seed, output, synth):
    output.mkdir(parents=True, exist_ok=False)
    command = [
        str(args.nextpnr), "--json", str(synth), "--device", "5CSEBA6U23I7",
        "--qsf", str(args.fixture / manifest["qsf"]), "--sdc", str(args.fixture / "clocks.sdc"),
        "--freq", "74.25", "--seed", str(seed), "--placer-heap-timingweight", "2000",
        "--placer-heap-critexp", "5", "--router", "gpu", "--gpu-device", str(args.gpu),
        "--report", str(output / "timing.json"), "--write", str(output / "routed.json"),
        "--rbf", str(output / "core.rbf"), "--compress-rbf", "--detailed-timing-report",
        "--timing-allow-fail",
    ]
    if args.gpu_cpu:
        command.append("--gpu-cpu")
    log_path = output / "nextpnr.log"
    started = time.monotonic()
    returncode, timed_out, spawn_error = None, False, None
    with log_path.open("w") as log:
        try:
            returncode = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                        timeout=args.timeout).returncode
        except subprocess.TimeoutExpired:
            timed_out = True
        except OSError as exc:
            spawn_error = str(exc)
    result = log_evidence(log_path.read_text(errors="replace"), returncode, timed_out)
    result.update({"seed": seed, "command": command, "log": str(log_path),
                   "wall_seconds": time.monotonic() - started, "spawn_error": spawn_error})
    return assess_run(args, output, result)


def assess_run(args, output, result):
    """Recheck a fresh or saved run's evidence without executing nextpnr."""
    result["errors"] = []
    result["rbf_sha256"] = None
    result["repeat_matches"] = None
    if result["outcome"] == "route_complete":
        result["outcome"] = "incomplete"
    expected_backend = "cpu-reference" if args.gpu_cpu else "hip:"
    backend = result["backend"] or ""
    backend_ok = backend == expected_backend if args.gpu_cpu else backend.startswith(expected_backend)
    result["backend_matches"] = backend_ok
    errors = []
    if result.get("spawn_error"):
        errors.append(f"could not start nextpnr: {result['spawn_error']}")
    artifacts = {name: (output / name).is_file() and (output / name).stat().st_size > 0
                 for name in ("core.rbf", "routed.json", "timing.json")}
    result["artifacts"] = artifacts
    if artifacts["core.rbf"]:
        result["rbf_sha256"] = sha256(output / "core.rbf")
    result["analogue_timing"], timing_errors = timing_evidence(output / "timing.json")
    result["timing_evidence_errors"] = timing_errors
    if args.expect_failure:
        expected_outcome = {"plateau": "initial_plateau", "packing": "packing_rejected"}[args.expect_failure]
        if result["outcome"] != expected_outcome:
            errors.append(f"expected {expected_outcome}, observed {result['outcome']}")
        if args.expect_failure == "plateau" and not backend_ok:
            errors.append(f"expected backend {expected_backend}, observed {result['backend']!r}")
    else:
        if result["returncode"] != 0 or result["timed_out"]:
            errors.append(f"tool did not succeed: {result['outcome']} (exit {result['returncode']})")
        for name in ("route_complete", "tool_complete", "signoff", "reached_zero_overuse"):
            if not result[name]:
                errors.append(f"missing {name} evidence")
        if not backend_ok:
            errors.append(f"expected backend {expected_backend}, observed {result['backend']!r}")
        if not result["last_logged_route_checksum"]:
            errors.append("no logged routing checksum")
        if result["tool_errors"]:
            errors.append("tool logged an ERROR despite completion evidence")
        errors += [f"missing or empty {name}" for name, present in artifacts.items() if not present]
        errors += timing_errors
        if not errors:
            result["outcome"] = "route_complete"
    result["errors"] = errors
    result["passed"] = not errors
    return result


def repeat_fingerprint(result):
    return {"last_logged_route_checksum": result["last_logged_route_checksum"],
            "rbf_sha256": result["rbf_sha256"],
            "fmax": result["analogue_timing"]["fmax"]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nextpnr", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--fixture", default="retained", help="retained, reconstructed, or a fixture directory")
    parser.add_argument("--seeds", type=int, nargs="+")
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=1800)
    parser.add_argument("--gpu", type=int, default=0)
    parser.add_argument("--gpu-cpu", action="store_true", help="explicitly qualify the CPU reference backend")
    parser.add_argument("--expect-failure", choices=("plateau", "packing"))
    args = parser.parse_args(argv)
    if (args.repeat < 1 or not math.isfinite(args.timeout) or args.timeout <= 0 or args.gpu < 0
            or (args.seeds is not None and any(seed < 1 for seed in args.seeds))):
        parser.error("repeat, timeout and seeds must be positive; gpu must be nonnegative")
    args.fixture = (FIXTURES / args.fixture if args.fixture in ("retained", "reconstructed")
                    else Path(args.fixture)).resolve()
    args.nextpnr = args.nextpnr.resolve()
    output = args.output.resolve()
    try:
        manifest, hashes, synth_bytes = load_fixture(args.fixture)
        binary_hash = sha256(args.nextpnr)
        output.mkdir(parents=True, exist_ok=False)
    except (OSError, ValueError, KeyError, TypeError) as exc:
        parser.error(str(exc))
    synth = output / "synth.json"
    synth.write_bytes(synth_bytes)
    summary = {"fixture": str(args.fixture), "fixture_hashes": hashes,
               "fixture_manifest_sha256": sha256(args.fixture / "fixture.json"),
               "nextpnr": str(args.nextpnr), "nextpnr_sha256": binary_hash,
               "expected_failure": args.expect_failure, "timeout_seconds": args.timeout, "runs": []}
    summary_path = output / "summary.json"
    for seed in dict.fromkeys(args.seeds or [manifest["default_seed"]]):
        previous = None
        for attempt in range(1, args.repeat + 1):
            destination = output / f"seed-{seed}-run-{attempt}"
            result = run(args, manifest, seed, destination, synth)
            result["attempt"] = attempt
            result["repeat_matches"] = None
            if result["outcome"] == "route_complete":
                fingerprint = repeat_fingerprint(result)
                if previous is not None:
                    result["repeat_matches"] = fingerprint == previous
                    if not result["repeat_matches"]:
                        result["errors"].append("routing checksum, RBF SHA-256 or final timing differs from previous repeat")
                        result["passed"] = False
                previous = fingerprint
            summary["runs"].append(result)
            summary["passed"] = all(item["passed"] for item in summary["runs"])
            # Persist each run immediately so a later interruption keeps evidence.
            summary_path.write_text(json.dumps(summary, indent=2, allow_nan=False) + "\n")
            timing = result["analogue_timing"]["fes_timing_pass"]
            timing_label = "unavailable" if timing is None else "PASS" if timing else "MISS"
            print(f"{'PASS' if result['passed'] else 'FAIL'}: Spectrum seed {seed} run {attempt}: "
                  f"{result['outcome']}; final analogue FES timing {timing_label}; see {result['log']}", flush=True)
    return 0 if summary["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
