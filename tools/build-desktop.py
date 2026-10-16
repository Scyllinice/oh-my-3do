#!/usr/bin/env python3
"""Build the native desktop app locally; no Python runtime is shipped."""
import argparse,os,platform,shutil,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--target-dir',type=Path);p.add_argument('--cargo');p.add_argument('--test',action='store_true')
args=p.parse_args()
cargo=args.cargo or (str(Path.home()/'.cargo/bin/cargo.exe') if os.name=='nt' else shutil.which('cargo'))
if not cargo:raise SystemExit('Install the stable Rust toolchain first')
target=args.target_dir or (Path(tempfile.gettempdir())/'oh3do-native-build' if os.name=='nt' else ROOT/'build/native-target')
target=target.resolve()
env=os.environ.copy()
flags=['--remap-path-prefix='+str(ROOT)+'=oh-my-3do','--remap-path-prefix='+str(Path.home()/'.cargo')+'=cargo']
if os.name=='nt':flags[:0]=['-C','target-feature=+crt-static']
env['CARGO_ENCODED_RUSTFLAGS']='\x1f'.join(flags)
if args.test:subprocess.run([cargo,'test','--locked','--target-dir',str(target)],cwd=ROOT/'desktop',env=env,check=True)
subprocess.run([cargo,'build','--release','--locked','--target-dir',str(target)],cwd=ROOT/'desktop',env=env,check=True)
print('Native app:',target/'release'/('oh-my-3do-library.exe' if os.name=='nt' else 'oh-my-3do-library'))
