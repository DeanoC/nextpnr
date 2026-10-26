#!/usr/bin/env python3
"""Retain validated Quartus primitive captures for offline regression and reproduction."""
import argparse
import gzip
import json
from pathlib import Path
import shutil
from reference import ROOT, CASES, CORNERS, sha, validate_case, load


def retain(capture, root=ROOT):
    capture, root = Path(capture), Path(root)
    receipt = json.loads((capture/"capture.json").read_text())
    assert receipt['device'] == "5CSEBA6U23I7" and set(receipt['cases']) == set(CASES)
    assert all(sha((ROOT/n).read_bytes()) == h for n,h in receipt['scripts_sha256'].items())
    validated = []
    for case in CASES:
        validated += validate_case(capture/case,case)
        assert all(c['returncode'] == 0 for c in receipt['cases'][case]['commands'])
        assert all(sha((capture/case/n).read_bytes()) == h for n,h in receipt['cases'][case]['artifact_sha256'].items())
    fixtures = root/"fixtures"
    fixtures.mkdir(parents=True)
    if root.resolve() != ROOT.resolve():
        for name in receipt['scripts_sha256']:
            shutil.copyfile(ROOT/name,root/name)
    manifest = dict(schema=1,device=receipt['device'],corners=list(CORNERS),
                    scope="Fitted primitive references only; no native timing model or physical qualification",
                    cases={},capture_sha256=sha((capture/"capture.json").read_bytes()),
                    producer_scripts_sha256=receipt['scripts_sha256'],quartus_version=receipt['quartus_version'],
                    decoder_sha256=receipt['decoder_sha256'],hardware=False)
    for case in CASES:
        source, target = capture/case, fixtures/case
        target.mkdir()
        names = ["fixture.json","fitted.rbf","fitted.bt","fitted.qsf","receipt.json","bindings.tsv"]
        names += [str(p.relative_to(source)) for p in (source/"source").iterdir()]
        names += [str(p.relative_to(source)) for p in (source/"arcs").glob("*.tsv")]
        names += [str(p.relative_to(source)) for p in (source/"paths").rglob("*.rpt")]
        for name in names:
            destination = target/name
            destination.parent.mkdir(parents=True,exist_ok=True)
            if name in ("fixture.json","fitted.rbf","fitted.bt","fitted.qsf","receipt.json") or name.endswith(".rpt"):
                Path(str(destination)+".gz").write_bytes(gzip.compress((source/name).read_bytes(),mtime=0))
            else:
                shutil.copyfile(source/name,destination)
        manifest['cases'][case] = {str(p.relative_to(target)):sha(p.read_bytes()) for p in target.rglob('*') if p.is_file()}
    reference = gzip.compress((json.dumps(validated,separators=(',',':'),sort_keys=True)+'\n').encode(),mtime=0)
    (root/"reference.json.gz").write_bytes(reference)
    manifest['reference_gzip_sha256'] = sha(reference)
    (root/"manifest.json").write_text(json.dumps(manifest,indent=2,sort_keys=True)+'\n')
    (root/"capture.json.gz").write_bytes(gzip.compress((capture/"capture.json").read_bytes(),mtime=0))
    assert load(root) == validated
    print(f"Retained {len(CASES)} cases and {len(validated)} complete primitive edge records")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture",type=Path)
    parser.add_argument("--root",type=Path,default=ROOT)
    args = parser.parse_args()
    retain(args.capture,args.root)
