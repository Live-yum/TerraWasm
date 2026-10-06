#!/usr/bin/env python3
"""Installed GCC/zlib only. ASan allocator contract; normal/UBSan also run Wasm32-pointer WLD operations."""
from pathlib import Path
import re,subprocess,concurrent.futures,sys,os
root=Path(__file__).resolve().parents[1];os.chdir(root)
asan='--asan' in sys.argv;ubsan='--ubsan' in sys.argv
build=root/('build-native-checkpoint-asan' if asan else 'build-native-checkpoint-ubsan' if ubsan else 'build-native-checkpoint');build.mkdir(exist_ok=True)
s=Path('CMakeLists.txt').read_text();files=[]
for name in ['COMMON_SOURCES','WLD_SOURCES']:
 files+=re.findall(r'\$\{SRC_DIR\}/([^"\s]+)',s.split('set('+name,1)[1].split('\n)',1)[0])
files+=['terra_zlib_bridge.c'];flags=(['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if asan else ['-fsanitize=undefined','-fno-omit-frame-pointer'] if ubsan else [])
def compile_file(file):
 args=['gcc','-std=gnu17','-O1','-g','-fno-pie','-DTERRAX_STATIC','-DTERRAX_TESTING','-DTERRAWASM_FEATURE_WLD=1','-DTERRAWASM_FEATURE_PLR=0','-Iinclude','-c','src/'+file,'-o',str(build/(file+'.o'))]
 if file=='terra_wld.c':args+=['-Dparse_header=terra_parse_header_unchecked']
 if file=='terra_txci.c':args+=['-DinflateInit2_=terra_inflateInit2_','-Dinflate=terra_inflate','-DinflateEnd=terra_inflateEnd']
 subprocess.run(args+flags,check=True)
with concurrent.futures.ThreadPoolExecutor(4) as e:list(e.map(compile_file,files))
subprocess.run(['ar','rcs',str(build/'libterra.a')]+[str(build/(f+'.o')) for f in files],check=True)
subprocess.run(['gcc','-std=gnu17','-O1','-g','-fno-pie','-no-pie','-DTERRAX_STATIC','-DTERRAX_TESTING','-Iinclude','tests/world_checkpoint_contract.c',str(build/'libterra.a'),'-lz','-lm','-o',str(build/'world_checkpoint_contract')]+flags,check=True)
os.environ.setdefault('UBSAN_OPTIONS','halt_on_error=1:print_stacktrace=1')
args=[str(build/'world_checkpoint_contract')]
if not asan:
 subprocess.run(['node','tests/generate-stream-fixtures.js',str(build/'fixtures')],check=True)
 args += [str(build/'fixtures'/'small.wld')]
subprocess.run(args,check=True)
