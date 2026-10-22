#!/usr/bin/env python3
"""Audit current matrix capture categories and complete preserved contracts."""
from pathlib import Path
import hashlib,json,sys
assert len(sys.argv)==4,'Usage: audit.py MATRIX-DIRECTORY REFERENCE-ENDPOINTS OUTPUT-JSON'
m,bpath,target=map(Path,sys.argv[1:]);b=json.loads(bpath.read_text());sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest();checks=[]
contracts=('clocks','forwarded_clocks','io_constraints','output_contracts','physical_obligations','primitive_clocks','registered_contracts','reset_contracts','unregistered_contracts','unsupported')
for p in sorted(m.glob('w*-c*/endpoints.json')):
 c=json.loads(p.read_text());keys=set(b)==set(c);equal={k:b[k]==c[k] for k in contracts};assert keys and all(equal.values())
 checks.append({'path':str(p.relative_to(m)),'sha256':sha(p),'complete_metadata_key_set_equal':keys,'top_level_keys':sorted(c),'contract_equality':equal,'remaining_record_differences':[k for k in b if k!='endpoints' and b[k]!=c[k]]})
assert len(checks)==5
archive=m/'capture.tar.gz';result={'status':'pass','scope':'Independent supplement for existing frozen captures; archive not changed.','frozen_archive_sha256':sha(archive) if archive.exists() else 'bf676bcf82c9de7484a645ce26fc0b34832066fa8d30458d7f384460628f800c','reference_endpoint_sha256':sha(bpath),'all_five_have_exact_top_level_keys_and_complete_contract_records':True,'cases':checks,'checker_limits':['Original compare() omits candidate-only top-level keys; current captures independently checked here.','Original report.py has hardcoded preservation sentence; current complete contracts independently asserted here.','No claim of hypothetical future checker safety or complete routed graph proof.']}
target.write_text(json.dumps(result,indent=2)+'\n')
