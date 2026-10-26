#!/usr/bin/env python3
"""Fit fixed LUT/carry cases and retain exact four-corner primitive edges and RBFs."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time
from project import CASES, sources

ROOT = Path(__file__).resolve().parent
CORNERS = ("7_slow_1100mv_100c", "7_slow_1100mv_-40c", "MIN_fast_1100mv_100c", "MIN_fast_1100mv_-40c")


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def characterize(output, quartus_bin, decoder, cases=CASES):
    output, quartus_bin, decoder = Path(output).absolute(), Path(quartus_bin).absolute(), Path(decoder).absolute()
    version = subprocess.check_output([str(quartus_bin/"quartus_sh"), "--version"], text=True)
    assert "Version 17.0.2 Build 602" in version, version
    output.mkdir(parents=True)
    result = dict(schema=1, device="5CSEBA6U23I7", quartus_version=version, cases={},
                  scope="Fixed primitive references, not native engine or physical qualification", hardware=False,
                  scripts_sha256={p.name: digest(p) for p in (ROOT/"project.py", ROOT/"extract.tcl", ROOT/"bindings.tcl", Path(__file__))},
                  decoder_sha256=digest(decoder))
    for case in cases:
        files, spec = sources(case)
        directory = output/case
        directory.mkdir()
        (directory/"source").mkdir()
        for name, text in files.items():
            (directory/name).write_text(text)
            (directory/"source"/name).write_text(text)
        (directory/"fixture.json").write_text(json.dumps(spec, indent=2)+"\n")
        shutil.copyfile(ROOT/"extract.tcl", directory/"extract.tcl")
        shutil.copyfile(ROOT/"bindings.tcl", directory/"bindings.tcl")
        receipt = dict(case=case, source_sha256={n: digest(directory/n) for n in files},
                       fixture_sha256=digest(directory/"fixture.json"), commands=[])
        commands = [(quartus_bin/"quartus_sh", ["--flow", "compile", "top"], "compile"),
                    (quartus_bin/"quartus_sta", ["-t", "extract.tcl"], "extract"),
                    (quartus_bin/"quartus_sta", ["-t", "bindings.tcl"], "bindings"),
                    (quartus_bin/"quartus_cpf", ["-c", "output_files/top.sof", "fitted.rbf"], "convert"),
                    (quartus_bin/"quartus_cdb", ["top", "--back_annotate=routing"], "annotate"),
                    (decoder, ["decomp", "5CSEBA6U23I7", "fitted.rbf", "fitted.bt"], "decode")]
        for tool, args, label in commands:
            started = time.monotonic()
            with (directory/(label+".log")).open("x") as stream:
                run = subprocess.run([str(tool), *args], cwd=directory, stdout=stream, stderr=subprocess.STDOUT)
            receipt['commands'].append(dict(command=[str(tool), *args], returncode=run.returncode,
                                             elapsed_seconds=time.monotonic()-started))
            (directory/"receipt.json").write_text(json.dumps(receipt, indent=2)+"\n")
            if run.returncode:
                raise RuntimeError(f"{case} {label} failed with {run.returncode}; retained {directory}")
        shutil.copyfile(directory/"top.qsf", directory/"fitted.qsf")
        assert set(p.stem for p in (directory/"arcs").glob("*.tsv")) == set(CORNERS)
        assert all((directory/"source"/n).read_text() == text for n, text in files.items())
        receipt["artifact_sha256"] = {str(p.relative_to(directory)): digest(p) for p in
            [directory/"fitted.rbf", directory/"fitted.bt", directory/"fitted.qsf", directory/"bindings.tsv",
             *(directory/"arcs").glob("*.tsv"), *(directory/"paths").rglob("*.rpt")]}
        (directory/"receipt.json").write_text(json.dumps(receipt, indent=2)+"\n")
        result["cases"][case] = receipt
        (output/"capture.json").write_text(json.dumps(result, indent=2)+"\n")
        print(f"{case}: fit, four-corner edges, decoded RBF and back-annotation complete", flush=True)
    assert result["scripts_sha256"] == {n: digest(ROOT/n) for n in result["scripts_sha256"]}
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--quartus-bin", type=Path, required=True)
    parser.add_argument("--decoder", type=Path, required=True)
    parser.add_argument("--cases", nargs="+", choices=CASES, default=CASES)
    args = parser.parse_args()
    characterize(args.output, args.quartus_bin, args.decoder, args.cases)
