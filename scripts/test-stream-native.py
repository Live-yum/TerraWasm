#!/usr/bin/env python3
"""Run source-only stream contracts using installed GCC, Node, and zlib.
No dependency downloads or Wasm artifact/manifest generation.
Use --ubsan for the Wasm32-pointer-compatible undefined-behavior run.
"""
from pathlib import Path
import re,subprocess,concurrent.futures,sys,os
os.chdir(Path(__file__).resolve().parents[1])
os.environ.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
flags=["-fsanitize=undefined","-fno-omit-frame-pointer"] if "--ubsan" in sys.argv else []
root=Path.cwd();build=root/('build-native-stream-ubsan' if flags else 'build-native-stream');build.mkdir(exist_ok=True)
s=Path('CMakeLists.txt').read_text();names=['COMMON_SOURCES','WLD_SOURCES'];files=[]
for name in names:
 part=s.split('set('+name,1)[1].split('\n)',1)[0]
 files+=re.findall(r'\$\{SRC_DIR\}/([^"\s]+)',part)
files+=['terra_zlib_bridge.c']
def compile_file(file):
 args=['gcc','-std=gnu17','-O1','-g','-fno-pie','-DTERRAX_STATIC','-DTERRAX_TESTING','-DTERRAWASM_FEATURE_WLD=1','-DTERRAWASM_FEATURE_PLR=0','-Iinclude','-c','src/'+file,'-o',str(build/(file+'.o'))]
 if file=='terra_wld.c':args+=['-Dparse_header=terra_parse_header_unchecked']
 if file=='terra_txci.c':args+=['-DinflateInit2_=terra_inflateInit2_','-Dinflate=terra_inflate','-DinflateEnd=terra_inflateEnd']
 if file=='terra_mem.c':args+=['-fno-tree-loop-distribute-patterns']
 subprocess.run(args+flags,check=True)
with concurrent.futures.ThreadPoolExecutor(6) as e:list(e.map(compile_file,files))
subprocess.run(['ar','rcs',str(build/'libterra.a')]+[str(build/(f+'.o')) for f in files],check=True)
print('built native archive',len(files))

subprocess.run(['node','tests/generate-stream-fixtures.js',str(build/'fixtures')],check=True)
subprocess.run(['gcc','-std=gnu17','-O1','-g','-fno-pie','-no-pie','-DTERRAX_STATIC','-DTERRAX_TESTING','-Iinclude',
    'tests/stream_boundary_contract.c',str(build/'libterra.a'),'-lz','-lm','-o',str(build/'stream_boundary_contract')]+flags,check=True)
subprocess.run([str(build/'stream_boundary_contract'),str(build/'fixtures')],check=True)
