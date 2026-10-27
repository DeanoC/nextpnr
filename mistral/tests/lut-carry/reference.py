#!/usr/bin/env python3
"""Validate fitted primitive references against complete arcs, routed inputs and truth tables."""
import argparse
import collections
import csv
from decimal import Decimal
import gzip
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import tempfile
from project import CASES, sources

ROOT = Path(__file__).resolve().parent
CORNERS = ("7_slow_1100mv_100c", "7_slow_1100mv_-40c", "MIN_fast_1100mv_100c", "MIN_fast_1100mv_-40c")
TRANSITIONS = ("rr", "rf", "fr", "ff")
MLAB_PERMUTE = (0,1,4,5,8,9,12,13,29,28,25,24,21,20,17,16,2,3,6,7,10,11,14,15,31,30,27,26,23,22,19,18,
                32,33,36,37,40,41,44,45,61,60,57,56,53,52,49,48,34,35,38,39,42,43,46,47,63,62,59,58,55,54,51,50)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def text(directory, name):
    path = Path(directory)/name
    return path.read_text() if path.exists() else gzip.decompress(Path(str(path)+".gz").read_bytes()).decode()


def rows(data):
    return list(csv.DictReader(io.StringIO(data), delimiter="\t"))


def decode(data):
    assert data.startswith("m 5CSEBA6U23I7\n")
    graph, settings = collections.defaultdict(set), {}
    for line in data.splitlines():
        fields = line.split()
        if fields and fields[0] == "r":
            graph[fields[1]].add(fields[2])
        elif fields and fields[0] == "s":
            assert fields[1] not in settings
            settings[fields[1]] = fields[2]
    return graph, settings


def routed_input(graph, location, site, kind):
    x,y,z = map(int, re.fullmatch(r"FF_X(\d+)_Y(\d+)_N(\d+)", location).groups())
    port = {1:"FFT0",2:"FFT1",4:"FFB0",5:"FFB1"}[z%6]
    roots = [f"{b}.{x:03d}.{y:03d}.{z//6}:{port}{suffix}" for b in ("LAB","MLAB") for suffix in ("","L")]
    target = f"{site['block']}.{site['x']:03d}.{site['y']:03d}.{site['alm']}:"
    pins = {"A","B",f"E{site['lane']}",f"F{site['lane']}"}
    pins |= {"C","D"} if kind.startswith("l6") else {"D" if site['lane'] else "C"}
    queue, seen, hits = collections.deque((r,[r]) for r in roots), set(), {}
    while queue:
        node, path = queue.popleft()
        if node in seen:
            continue
        seen.add(node)
        if node.startswith(target) and node[len(target):] in pins:
            hits[node[len(target):]] = path
            continue
        for child in sorted(graph[node]):
            queue.append((child,path+[child]))
    assert len(hits) == 1, (site['cell'], location, hits)
    pin, path = next(iter(hits.items()))
    return pin, path


def pin_value(pin, bit, l6):
    if pin == "A": return bit & 1
    if pin == "B": return (bit >> 1) & 1
    if pin == "C": return (bit >> (3 if l6 and bit >= 32 else 2)) & 1
    if pin == "D": return (bit >> (3 if l6 and bit < 32 else 2)) & 1
    if pin.startswith("E"): return (bit >> (5 if l6 else 3)) & 1
    if pin.startswith("F"): return (bit >> 4) & 1
    raise AssertionError(pin)


def check_truth(site, kind, mapping, settings):
    key = f"{site['block']}.{site['x']:03d}.{site['y']:03d}:LUT_MASK.{site['alm']}"
    assert key in settings
    actual = int(settings[key].replace(".",""),16)
    if site['block'] == "MLAB":
        actual = sum(((actual >> j) & 1) << i for i,j in enumerate(MLAB_PERMUTE))
    l6, carry = kind.startswith("l6"), kind == "carry"
    source = int(site['mask'],16)
    swapped = False
    if carry:
        assert {mapping[3][0],mapping[4][0]} == {f"E{site['lane']}",f"F{site['lane']}"}
        swapped = mapping[3][0].startswith("F")
    expected, touched = 0, 0
    for bit in range(64 if l6 else 32):
        native = bit if l6 else bit+32*site['lane']
        if carry:
            second = (bit >= 16) != swapped
            indices = (0,1,2,4 if second else 3)
            # D0 and D1 select the same bit in their respective 16-bit tables.
            values = [(bit >> 3) & 1 if mapping[i][0].startswith(("E","F"))
                      else pin_value(mapping[i][0],bit,False) for i in indices]
            index = sum((1-v) << p for p,v in enumerate(values))
            value = (source >> (32 if second else 0) >> index) & 1
        else:
            indices = range(6) if l6 else (0,1,2,3,4+site['lane'])
            index = sum((1-pin_value(mapping[i][0],bit,l6)) << p for p,i in enumerate(indices))
            value = (source >> index) & 1
        # Arithmetic u + !v + cin also permits !v + u + cin. The fitted
        # routed private inputs identify that operand exchange independently;
        # its two complemented tables are checked against the same source.
        expected |= (value if carry and swapped else 1-value) << native
        touched |= 1 << native
    assert actual & touched == expected, (site['cell'], hex(actual), hex(expected), kind)


def validate_case(directory, case):
    directory = Path(directory)
    files, spec = sources(case)
    assert json.loads(text(directory,"fixture.json")) == spec
    for name, value in files.items():
        assert text(directory,"source/"+name) == value, (case,name)
    graph, settings = decode(text(directory,"fitted.bt"))
    qsf = text(directory,"fitted.qsf")
    locations = {int(i):loc for loc,i in re.findall(
        r"set_location_assignment (FF_X\d+_Y\d+_N\d+) -to \"?launch_(\d+)\"?(?:\n|$)",qsf)}
    kind = case.split("-",1)[1]
    sites = {s['cell']:s for s in spec['sites']}
    bindings = rows(text(directory,"bindings.tsv"))
    bound, normalized, physicals = {}, [], {}
    for b in bindings:
        assert b['cell'] in sites and b['input'] != "cin"
        m = re.fullmatch(r"launch_(\d+)",b['launch']); assert m,b
        i = int(m[1]); assert locations[i] == b['launch_location']
        key = (b['cell'],b['input']); assert key not in bound
        pin,path = routed_input(graph,b['launch_location'],sites[b['cell']],kind)
        bound[key] = dict(source_index=i,physical=pin,route=path)
        mapping = physicals.setdefault(b['cell'],{})
        assert i not in mapping
        mapping[i] = (pin,path)
    for s in sites.values():
        expected = set(range(5 if kind == "carry" else 6))
        if kind == "l5": expected = {0,1,2,3,4+s['lane']}
        assert set(physicals[s['cell']]) == expected, (case,s['cell'],physicals[s['cell']])
        assert len({p[0] for p in physicals[s['cell']].values()}) == len(expected)
        base = f"{s['block']}.{s['x']:03d}.{s['y']:03d}:"
        if kind == "carry":
            assert settings[base+f"ARITH_SEL.{s['alm']}"] == "ADDER"
        for field, value in (("TMODE","E_0"),("BMODE","E_1")):
            assert settings.get(base+f"{field}.{s['alm']}","D_E" if field == "TMODE" else "C_E") == (
                "D_E" if field == "TMODE" and kind.startswith("l6") else
                "C_E" if field == "BMODE" and kind.startswith("l6") else value)
        check_truth(s,kind,physicals[s['cell']],settings)
    signatures, arc_index = [], {}
    for corner in CORNERS:
        seen = set()
        for r in rows(text(directory,"arcs/"+corner+".tsv")):
            s = sites[r['cell']]
            assert r['location'] == s['location'] and r['wysiwyg'] == "cyclonev_lcell_comb"
            assert r['src'] == r['cell']+"|"+r['input'] and r['dst'] == r['cell']+"|"+r['output']
            key = (r['cell'],r['input'],r['output']); assert key not in seen
            seen.add(key)
            assert r['output'] in (("sumout","cout") if kind == "carry" else ("combout",))
            b = bound.get(key[:2])
            if not b:
                assert kind == "carry" and r['input'] == "cin" and r['cell'] != "target_0"
            if b and kind != "carry":
                ordinal = b['source_index']
                if kind == "l5" and ordinal == 5: ordinal = 4
                truth = int(s['mask'],16)
                signs = {(truth >> (i|(1<<ordinal)) & 1)-(truth >> i & 1)
                         for i in range(1<<s['width']) if not (i&(1<<ordinal))}
                signs.discard(0)
                assert signs and r['unate'] == ("positive" if signs == {1} else "negative" if signs == {-1} else "unknown")
            valid = ("rr","ff") if r['unate'] == "positive" else ("rf","fr") if r['unate'] == "negative" else TRANSITIONS
            assert r['unate'] in ("positive","negative","unknown")
            delays = {}
            for rf in TRANSITIONS:
                values = [r[rf+"_"+mode] for mode in ("min","max")]
                assert all(values) == (rf in valid) and (all(values) or not any(values)), r
                if rf in valid:
                    values = [Decimal(v)*1000 for v in values]
                    assert all(v.is_finite() and v == int(v) for v in values) and 0 <= values[0] <= values[1]
                    delays[rf] = [int(v) for v in values]
            normalized.append(dict(case=case,corner=corner,cell=r['cell'],block=s['block'],lane=s['lane'],
                alm=s['alm'],input=r['input'],output=r['output'],unate=r['unate'],delays_ps=delays,
                physical=b['physical'] if b else "CI",source_index=b['source_index'] if b else None))
            arc_index[(corner,*key)] = delays
        expected = {(cell,pin,out) for cell,pin in bound for out in (("sumout","cout") if kind == "carry" else ("combout",))}
        if kind == "carry":
            expected |= {(f"target_{i}","cin",out) for i in range(1,20) for out in ("sumout","cout")}
        assert seen == expected, (case,corner,expected-seen,seen-expected)
        signatures.append(seen)
    assert all(s == signatures[0] for s in signatures)
    check_paths(directory,sites,arc_index)
    return sorted(normalized,key=lambda r:(r['case'],r['corner'],r['cell'],r['input'],r['output']))


def check_paths(directory, sites, arcs):
    """Independently match retained path CELL increments to direct edge queries."""
    for corner in CORNERS:
        for analysis in ("setup","hold"):
            for edge in ("rise","fall"):
                data = text(directory,f"paths/{corner}/{analysis}-{edge}.rpt")
                assert "Data Arrival Path" in data and "Data Required Path" in data
                observed, previous = set(), None
                for line in data.splitlines():
                    if not line.startswith(';'): continue
                    fields = [s.strip() for s in line.strip(';').split(';')]
                    if len(fields) != 7: continue
                    try: increment = Decimal(fields[1])*1000
                    except ArithmeticError: continue
                    match = re.fullmatch(r"(target_\d+)\|(combout|sumout|cout)",fields[6])
                    if fields[3] == "CELL" and match:
                        cell, output = match.groups()
                        assert cell in sites and fields[5] == sites[cell]['location']
                        assert previous is not None and previous[3] == "IC"
                        source = re.fullmatch(re.escape(cell)+r"\|(data[a-f]|cin)",previous[6])
                        assert source,(cell,previous)
                        key = (corner,cell,source[1],output)
                        assert key in arcs and fields[2].lower() in arcs[key]
                        expected = arcs[key][fields[2].lower()][int(analysis == "setup")]
                        assert abs(increment-expected) <= 1,(key,analysis,increment,expected)
                        observed.add(cell)
                    previous = fields
                assert observed == set(sites),(corner,analysis,edge,set(sites)-observed)


def load(root=ROOT, decoder=None):
    root = Path(root)
    manifest = json.loads((root/"manifest.json").read_text())
    assert manifest['schema'] == 1 and manifest['device'] == "5CSEBA6U23I7"
    assert manifest['corners'] == list(CORNERS) and set(manifest['cases']) == set(CASES)
    capture = gzip.decompress((root/"capture.json.gz").read_bytes())
    assert sha(capture) == manifest['capture_sha256']
    provenance = json.loads(capture)
    assert provenance['device'] == manifest['device'] and provenance['scripts_sha256'] == manifest['producer_scripts_sha256']
    assert all(sha((root/n).read_bytes()) == h for n,h in manifest['producer_scripts_sha256'].items())
    result = []
    for case in CASES:
        files = manifest['cases'][case]
        directory = root/"fixtures"/case
        required = {"fixture.json.gz","fitted.rbf.gz","fitted.bt.gz","fitted.qsf.gz","bindings.tsv","receipt.json.gz"}
        required |= {"source/"+n for n in sources(case)[0]}
        required |= {f"arcs/{c}.tsv.gz" for c in CORNERS}
        required |= {f"paths/{c}/{a}-{e}.rpt.gz" for c in CORNERS for a in ("setup","hold") for e in ("rise","fall")}
        assert set(files) == required,(case,required-set(files),set(files)-required)
        assert set(str(p.relative_to(directory)) for p in directory.rglob('*') if p.is_file()) == set(files)
        for name, digest in files.items():
            assert sha((directory/name).read_bytes()) == digest,(case,name)
        receipt = json.loads(text(directory,"receipt.json"))
        assert receipt == provenance['cases'][case] and all(c['returncode'] == 0 for c in receipt['commands'])
        for name,digest in receipt['artifact_sha256'].items():
            path = directory/name
            data = path.read_bytes() if path.exists() else gzip.decompress(Path(str(path)+".gz").read_bytes())
            assert sha(data) == digest,(case,name,"producer binding")
        if decoder:
            with tempfile.TemporaryDirectory() as d:
                rbf, bt = Path(d)/"fitted.rbf", Path(d)/"fitted.bt"
                rbf.write_bytes(gzip.decompress((directory/"fitted.rbf.gz").read_bytes()))
                subprocess.run([str(Path(decoder).resolve()),"decomp",manifest['device'],str(rbf),str(bt)],check=True)
                assert bt.read_text() == text(directory,"fitted.bt"),(case,"RBF decode differs")
        result += validate_case(directory,case)
    reference = (root/"reference.json.gz").read_bytes()
    assert sha(reference) == manifest['reference_gzip_sha256']
    expected = json.loads(gzip.decompress(reference))
    assert expected == result
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root",type=Path,default=ROOT)
    parser.add_argument("--decoder",type=Path)
    args = parser.parse_args()
    values = load(args.root,args.decoder)
    print(f"{len(CASES)} fitted cases; {len(values)} complete four-corner primitive arcs; routed inputs and truth tables agree")
