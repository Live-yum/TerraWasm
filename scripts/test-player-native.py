#!/usr/bin/env python3
"""Source-native PLR contracts with installed GCC only; never produces Wasm."""
from pathlib import Path
import os, subprocess, sys, hashlib
os.chdir(Path(__file__).resolve().parents[1])
flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if '--sanitize' in sys.argv else []
build=Path('build-native-player-sanitize' if flags else 'build-native-player');build.mkdir(exist_ok=True)
base=['gcc','-std=gnu17','-O1','-g','-fno-pie','-DTERRAX_STATIC','-DTERRAX_TESTING','-DTERRAWASM_FEATURE_WLD=0','-DTERRAWASM_FEATURE_PLR=1','-Iinclude']+flags
objects=[]
for name in ['terra_mem','terra_reader','terra_abi','terra_plr']:
 out=build/(name+'.o');subprocess.run(base+(['-fno-tree-loop-distribute-patterns'] if name=='terra_mem' else [])+['-c','src/'+name+'.c','-o',str(out)],check=True);objects.append(str(out))
for name in ['plr_contract','plr_workspace_contract','plr_workspace_differential']:
 out=build/name
 subprocess.run(base+['-no-pie','-DTERRAWASM_PLR_JSON_FIXTURE_PATH="'+str(Path('tests/fixtures/minimal-player.json').resolve())+'"','tests/'+name+'.c',*objects,'-lm','-o',str(out)],check=True)
 if name == 'plr_workspace_differential':
  result=subprocess.check_output([str(out)])
  expected='cbd7b51b0e149e578a0b500ae45048ba3a2a421c5e4bc9b9074e6e81d1f529c2'
  actual=hashlib.sha256(result).hexdigest()
  if actual != expected: raise RuntimeError('Pre-workspace codec golden differs: '+actual)
  print('PLR codec golden: 256 batches, 256 rollbacks, 16 encrypted exports;',len(result),'bytes SHA-256',actual)
 else: subprocess.run([str(out)],check=True)
