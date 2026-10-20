#!/usr/bin/env python3
import gzip,os,subprocess,sys,tempfile
from pathlib import Path
here=Path(__file__).resolve().parent
binary=Path(sys.argv[1]).resolve();output=Path(sys.argv[2]).resolve()
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp)/'placed.json';p.write_bytes(gzip.decompress((here/'host-inputs/placed.json.gz').read_bytes()))
 paths={'IO_CHECKPOINT':p,'IO_SDC':here/'host-inputs/synthetic-io.sdc','REGISTERED_OVERLAY':here/'host-inputs/reference-profile-overlay.json','REGISTERED_SDC':here/'host-inputs/registered-synthetic.sdc','FORWARD_OVERLAY':here/'host-inputs/forward-overlay.json','RESET_OVERLAY':here/'host-inputs/reset-overlay.json','UNREGISTERED_OVERLAY':here/'host-inputs/dq-overlay.json','IO_DIAGNOSTIC':output}
 env=dict(os.environ,**{'NEXTPNR_TEST_'+key:str(value) for key,value in paths.items()})
 subprocess.run([str(binary),'--gtest_filter=ExternalEndpointDiagnostic.PlacedCheckpointSyntheticInputConstraints'],env=env,check=True)
