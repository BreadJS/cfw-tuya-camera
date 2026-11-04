#!/usr/bin/env python3
"""FD handoff tests using real locks and signed disposable host fixtures."""
import subprocess,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests/support'))
from fixtures import ApplyFixtures as ApplyTests
with tempfile.TemporaryDirectory(prefix='camera-plan-handoff-') as tmp:
    binary=Path(tmp)/'driver';src=ROOT/'src/ota'
    subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(src/'vendor'),str(Path(__file__).with_name('driver.c')),*[str(src/x) for x in ['ota_apply_plan.c','ota_state.c','ota_core.c','vendor/monocypher.c','vendor/monocypher-ed25519.c']],'-o',str(binary)],check=True)
    for scenario in ['valid','wrong-lock','same-directories','corrupt-capsule']:
        case=ApplyTests();case.setUp()
        try:
            subprocess.run([str(binary),str(case.pub),str(case.state),str(case.stage),str(case.map),scenario],check=True,timeout=15)
        finally:case.tearDown()
print('PASS4 signed-plan handoff cases: retained flock after service close, lock identity, directory separation and capsule corruption')
