#!/usr/bin/env python3
"""Replay an immutable capture; distinguish JSON value equality from key rendering order."""
from pathlib import Path
import argparse, gzip, hashlib, json, os, subprocess, tarfile

ARCHIVE_SHA = '18c8074b7fedba429cabec99d2a337a64b827506a12a0f7d2b06d1d43ae7b237'
MANIFEST_SHA = '0ef043d10bcae67c8d94dce8ec28acec43c95f6540fd233006d379d52128214b'

def need(ok, text):
    if not ok:
        raise ValueError(text)

def digest(data):
    return hashlib.sha256(data).hexdigest()

def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def unique(items):
    out = {}
    for key, value in items:
        need(key not in out, 'duplicate JSON key ' + key)
        out[key] = value
    return out

def parse(data):
    return json.loads(data, object_pairs_hook=unique)

def canonical(data):
    return (json.dumps(data, sort_keys=True, separators=(',', ':'), ensure_ascii=True) + '\n').encode()

def order_changes(a, b, path=()):
    need(type(a) is type(b), 'JSON type changed at ' + repr(path))
    out = []
    if isinstance(a, dict):
        need(a.keys() == b.keys(), 'dictionary keys changed at ' + repr(path))
        if list(a) != list(b):
            out.append({'path': list(path), 'captured_order': list(a), 'replayed_order': list(b)})
        for key in sorted(a):
            out.extend(order_changes(a[key], b[key], path + (key,)))
    elif isinstance(a, list):
        need(len(a) == len(b), 'array length changed at ' + repr(path))
        for i, (left, right) in enumerate(zip(a, b)):
            out.extend(order_changes(left, right, path + (i,)))
    else:
        need(a == b, 'JSON value changed at ' + repr(path))
    return out

def compare(old, new, label, output):
    a, b = parse(old), parse(new)
    changes = order_changes(a, b)
    need(canonical(a) == canonical(b), 'canonical payload changed')
    for change in changes:
        path = change['path']
        need(len(path) == 3 and path[0] in ['every_endpoint_row', 'every_previously_failing_endpoint_with_actual_saved_source']
             and type(path[1]) is int and path[2] == 'margin_delta_ps', 'unexpected rendering permutation')
        need(set(change['captured_order']) == {'setup', 'hold', 'min_path', 'max_path'}, 'unexpected margin keys')
    payload = canonical(changes)
    name = label + '-key-order-changes.json.gz'
    (output / name).write_bytes(gzip.compress(payload, compresslevel=9, mtime=0))
    first = next((i for i, (x, y) in enumerate(zip(old, new)) if x != y), None)
    if first is None and len(old) != len(new):
        first = min(len(old), len(new))
    return {'label': label, 'parsed_types_keys_values_and_array_order_equal': True,
            'byte_identical': old == new, 'captured_sha256': digest(old), 'replayed_sha256': digest(new),
            'captured_bytes': len(old), 'replayed_bytes': len(new), 'first_differing_byte_offset': first,
            'canonical_sha256': digest(canonical(a)), 'canonical_bytes': len(canonical(a)),
            'dictionary_order_changes': len(changes), 'complete_key_permutations_file': name,
            'complete_key_permutations_sha256': sha(output / name),
            'complete_key_permutations_uncompressed_sha256': digest(payload),
            'path_category_counts': {category: sum(x['path'][0] == category for x in changes)
                for category in ['every_endpoint_row', 'every_previously_failing_endpoint_with_actual_saved_source']}}

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--observed-root-replay', type=Path)
    args = parser.parse_args()
    need(sha(args.archive) == ARCHIVE_SHA, 'original archive identity changed')
    args.output.mkdir(parents=True, exist_ok=True)
    args.work.mkdir(parents=True, exist_ok=False)
    with tarfile.open(args.archive, 'r:gz') as archive:
        members = archive.getmembers()
        names = [member.name for member in members]
        need(len(set(names)) == len(names), 'duplicate archive members')
        manifest_bytes = archive.extractfile('manifest.json').read()
        need(digest(manifest_bytes) == MANIFEST_SHA, 'manifest identity changed')
        manifest = parse(manifest_bytes)
        need(set(names) == set(manifest) | {'manifest.json'}, 'archive inventory changed')
        for member in members:
            need(member.isfile() and not Path(member.name).is_absolute() and '..' not in Path(member.name).parts, 'unsafe member')
            data = archive.extractfile(member).read()
            if member.name != 'manifest.json':
                need(len(data) == manifest[member.name]['size'] and digest(data) == manifest[member.name]['sha256'], 'member digest mismatch')
            dest = args.work / member.name
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
    folder = args.work / 'final-analysis'
    checks = [('prove-graphs.py', 'graph-proof.json'), ('compare-routes.py', 'route-comparison.json'),
              ('repair-decisions.py', 'repair-decisions.json')]
    originals = {name: (folder / name).read_bytes() for _, name in checks}
    source = (folder / 'compare-routes.py').read_text()
    need("margins={'setup','hold','min_path','max_path'}" in source and
         "for m in margins}" in source, 'expected insertion-order source changed')
    outputs = []
    if args.observed_root_replay:
        outputs.append(compare(originals['route-comparison.json'], args.observed_root_replay.read_bytes(),
                               'root-observed', args.output))
    for seed in [0, 1, 2]:
        for script, result in checks if seed == 0 else checks[1:2]:
            label = script[:-3] + '-seed-' + str(seed)
            env = dict(os.environ, PYTHONHASHSEED=str(seed))
            with (args.output / (label + '.log')).open('wb') as log:
                process = subprocess.run(['python3', str(folder / script)], env=env, stdout=log, stderr=subprocess.STDOUT)
            need(process.returncode == 0, 'replay script failed: ' + label)
            check = compare(originals[result], (folder / result).read_bytes(), label, args.output)
            check.update(script=script, result=result, pythonhashseed=seed, returncode=process.returncode,
                         stdout_sha256=sha(args.output / (label + '.log')))
            outputs.append(check)
    result = {'status': 'pass', 'archive_sha256': ARCHIVE_SHA, 'manifest_sha256': MANIFEST_SHA,
              'archive_unchanged_after_replay': sha(args.archive) == ARCHIVE_SHA,
              'verified_members': len(names), 'checker_sha256': sha(Path(__file__)),
              'cause': 'The captured compare-routes.py creates margin_delta_ps dictionaries by iterating a four-element Python set. PYTHONHASHSEED changes dictionary insertion order; json.dumps preserves that rendering order.',
              'source_lines': [84, 87], 'checks': outputs,
              'limits': 'Full original report values, types, keys, array order and canonical bytes are compared. Only the enumerated margin dictionary key permutations are permitted. No native route or hardware run is performed; failed timing results are unchanged.'}
    need(result['archive_unchanged_after_replay'], 'archive changed during replay')
    (args.output / 'result.json').write_bytes(canonical(result))
    print(json.dumps({'status': 'pass', 'checks': len(outputs), 'archive_sha256': ARCHIVE_SHA}, indent=2))

if __name__ == '__main__':
    main()
