#!/usr/bin/env python3
"""Create a local diagnostic .fcore bundle, preserving embedded hardware identities.

This is a post-route experimental envelope, not a fresh producer export/seal.
Original synthesis provenance and the separate reroute receipt are retained.
"""
import argparse
import copy
import hashlib
import importlib
import json
from pathlib import Path
import sys
import tarfile
import io


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--passing-root', type=Path, required=True)
    parser.add_argument('--failing-root', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True, help='Completed legacy_reroute.py output directory')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--consumer-results', type=Path, help='Optional JSONL output from FogCast InspectPackage for these three archives')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    reference = json.loads((here/'baseline-reference.json').read_text())
    reroute_bytes = (args.candidate/'receipt.json').read_bytes()
    reroute = json.loads(reroute_bytes)
    candidate = (args.candidate/'candidate.rbf').read_bytes()
    if sha(candidate) != reroute['candidate_rbf_sha256'] or reroute['changed_routes'] != sorted(
            f'sdram.sdram_a[{bit}]' for bit in (9, 10, 12)):
        raise ValueError('Candidate does not match the controlled reroute receipt')
    if reroute['original_failing_rbf_sha256'] != reference['builds']['failing']['original_rbf_sha256']:
        raise ValueError('Wrong historical baseline')
    # Use the historical implementation's manifest encoder and strict reader.
    # Do not edit its checkout or manufacture a new synthesis/build-input receipt.
    sys.path.insert(0, str(args.failing_root.resolve()))
    package = importlib.import_module('scripts.core_package')
    args.output.mkdir(parents=True, exist_ok=True)
    records = {}
    failing_fields = None
    for label, root in [('passing', args.passing_root), ('failing', args.failing_root)]:
        folder = root/'build/fes-ramtest-100'
        manifest = (folder/'manifest.toml').read_bytes()
        payload = (folder/'core.rbf').read_bytes()
        if sha(payload) != reference['builds'][label]['original_rbf_sha256']:
            raise ValueError(f'Wrong {label} historical RBF')
        fields = package._decode_manifest(manifest, payload)
        if fields['core']['id'] != 'fes.ramtest' or fields['format'] != 2:
            raise ValueError('Expected historical format-2 ramtest')
        records[label] = (manifest, payload, fields)
        if label == 'failing':
            failing_fields = fields
    fields = copy.deepcopy(failing_fields)
    fields['core']['name'] = 'FES RAM Tester — Address Route Control'
    fields['core']['version'] = '1.1.0-address-route-control.1'
    fields['core']['description'] = 'Experimental 100 MHz comparison; historical RTL with only A9/A10/A12 rerouted; not hardware validated'
    fields['payload']['size'] = len(candidate)
    fields['payload']['sha256'] = sha(candidate)
    fields['build']['toolchain'] += '; diagnostic post-route CPU router2 estimateWeight=0 seed=2 nextpnr-sha256=' + reroute['nextpnr_sha256']
    # build.id is physically embedded in unchanged RTL, not a package digest.
    records['address-control'] = (package.encode_manifest(fields), candidate, fields)
    receipt = dict(classification='local experimental packaging only; no producer reseal, host import or hardware programming',
                   reroute_receipt_sha256=sha(reroute_bytes),
                   packaging_source_sha256=sha((args.failing_root/'scripts/core_package.py').read_bytes()),
                   script_sha256=sha(Path(__file__).read_bytes()), packages={})
    for label, (manifest, payload, fields) in records.items():
        def member(name, data):
            return package._ustar_header(name, len(data))+data+b'\0'*((-len(data)) % 512)
        archive = member('manifest.toml', manifest)+member('core.rbf', payload)+b'\0'*1024
        path = args.output/f'{label}.fcore'
        path.write_bytes(archive)
        parsed = package.read_package(path)
        if parsed.fields != fields or parsed.package_id != package.package_identity(manifest, payload):
            raise ValueError('Archive round trip failed')
        receipt['packages'][label] = dict(file=path.name, package_id=parsed.package_id,
                                        archive_sha256=sha(archive), payload_sha256=sha(payload),
                                        embedded_build_id=fields['build']['id'], version=fields['core']['version'])
    assert receipt['packages']['address-control']['embedded_build_id'] == receipt['packages']['failing']['embedded_build_id']
    assert len({p['package_id'] for p in receipt['packages'].values()}) == 3
    if args.consumer_results:
        consumer_bytes = args.consumer_results.read_bytes()
        inspected = [json.loads(line) for line in consumer_bytes.splitlines()]
        if len(inspected) != 3 or {r['package_id'] for r in inspected} != {r['package_id'] for r in receipt['packages'].values()}:
            raise ValueError('Consumer did not inspect the exact three packages')
        for result in inspected:
            expected = next(r for r in receipt['packages'].values() if r['package_id'] == result['package_id'])
            if result['descriptor']['payload']['sha256'] != expected['payload_sha256']:
                raise ValueError('Consumer payload identity mismatch')
        receipt['consumer_validation'] = dict(reader='FogCast corepackage.InspectPackage',
                                              inspected_packages=3, results_sha256=sha(consumer_bytes))
        (args.output/'consumer-results.jsonl').write_bytes(consumer_bytes)
    (args.output/'reroute-receipt.json').write_bytes(reroute_bytes)
    (args.output/'package-receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
    results = dict(status='awaiting_hardware', kit=None, boot_id=None, rate_mhz=100,
                   runs=[], expected_packages=receipt['packages'])
    (args.output/'hardware-results-template.json').write_text(json.dumps(results, indent=2)+'\n')
    lines = ['# Historical 100 MHz SDRAM route comparison', '',
             'These packages are local diagnostics. No hardware result is claimed.',
             'The address-control package changes only A9/A10/A12 routes in the historical failing RBF.',
             'Original ABI, FPGA-embedded build ID and capture schedule are preserved.',
             'Its package ID and prerelease label distinguish it from the historical control.',
             'This envelope is not a new producer synthesis seal; reroute-receipt.json records post-processing.', '',
             '| Label | Package ID | Payload SHA-256 |', '| --- | --- | --- |']
    lines += [f"| {label} | {r['package_id']} | {r['payload_sha256']} |" for label, r in receipt['packages'].items()]
    lines += ['', 'On the FogCast host, import each .fcore and create an entry with its exact package ID:', '',
              '```sh', 'fogcast --api http://127.0.0.1:8787 --json core-install /absolute/path/LABEL.fcore',
              "fogcast --api http://127.0.0.1:8787 --json core-entry 'RAM route test LABEL' PACKAGE_ID", '```', '',
              'Launch entries through the ordinary library. Stop the current core before each launch.',
              'Keep the same kit, SDRAM board and boot. Run failing → address-control → failing;',
              'the passing package is an additional baseline check if the controls do not reproduce.',
              'Check the selected package/payload digest in runtime status before recording a run.',
              'Complete all six SDRAM patterns, and record per-pattern errors, first fault/observed word',
              'and HPS DDR outcome. Record kit, boot ID, run order and elapsed time in a copy of',
              'hardware-results-template.json; include screenshots/status evidence.',
              'A candidate pass with reproducible failing controls implicates this address-route group.',
              'A candidate failure does not exclude other address setup, read capture or command skew.', '']
    (args.output/'README.md').write_text('\n'.join(lines))
    files = sorted(p for p in args.output.iterdir() if p.is_file() and p.name not in ('SHA256SUMS', 'ramtest-address-comparison.tar'))
    (args.output/'SHA256SUMS').write_text(''.join(f'{sha(p.read_bytes())}  {p.name}\n' for p in files))
    with tarfile.open(args.output/'ramtest-address-comparison.tar', 'w', format=tarfile.USTAR_FORMAT) as tar:
        for path in files+[args.output/'SHA256SUMS']:
            data = path.read_bytes()
            info = tarfile.TarInfo(path.name)
            info.size, info.mode = len(data), 0o644
            tar.addfile(info, io.BytesIO(data))
    print(json.dumps(receipt['packages'], indent=2))


if __name__ == '__main__':
    main()
