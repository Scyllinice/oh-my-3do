#!/usr/bin/env python3
"""Build the native desktop app locally; no Python runtime is shipped."""
import argparse,json,os,shutil,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
WINDOWS='x86_64-pc-windows-msvc'
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--target-dir',type=Path);p.add_argument('--cargo');p.add_argument('--test',action='store_true')
p.add_argument('--windows',action='store_true',help='cross-compile for Windows x64 (MSVC) in Docker with cargo-xwin; accepts Microsoft\'s CRT/SDK license on download')
args=p.parse_args()
subprocess.run([os.environ.get("PYTHON", "python3"), str(ROOT/"tools/build-save-cheats.py"), "--check"], check=True)

def cross_windows():
    docker=shutil.which('docker')
    if not docker:raise SystemExit('Install Docker first')
    image='oh3do-desktop-windows'
    with open(ROOT/'tools/desktop-windows.Dockerfile','rb') as dockerfile:subprocess.run([docker,'build','-t',image,'-'],stdin=dockerfile,check=True)
    target=(args.target_dir or ROOT/'build/windows-target').resolve();cache=ROOT/'build/xwin-cache'
    target.mkdir(parents=True,exist_ok=True);cache.mkdir(parents=True,exist_ok=True)
    flags=['-C','target-feature=+crt-static','--remap-path-prefix=/src=oh-my-3do','--remap-path-prefix=/cache/cargo=cargo']
    run=[docker,'run','--rm','--user',f'{os.getuid()}:{os.getgid()}','-v',f'{ROOT}:/src','-v',f'{target}:/target','-v',f'{cache}:/cache','-w','/src/desktop',
         '-e','HOME=/cache','-e','CARGO_HOME=/cache/cargo','-e','XWIN_CACHE_DIR=/cache/xwin','-e','XWIN_ACCEPT_LICENSE=1',
         '-e','CARGO_ENCODED_RUSTFLAGS='+'\x1f'.join(flags),image,'cargo','xwin']
    common=['--locked','--target',WINDOWS,'--target-dir','/target']
    if args.test:
        out=subprocess.run(run+['test','--no-run','--message-format=json-render-diagnostics']+common,check=True,stdout=subprocess.PIPE,text=True).stdout
        tests=[target/Path(m['executable']).relative_to('/target') for m in map(json.loads,out.splitlines()) if m.get('reason')=='compiler-artifact' and m.get('executable') and m['profile']['test']]
        if Path('/proc/sys/fs/binfmt_misc/WSLInterop').exists():
            for test in tests:subprocess.run([str(test)],check=True)
        else:print('Not under WSL; run these test executables on Windows:',*tests,sep='\n  ')
    subprocess.run(run+['build','--release']+common,check=True)
    print('Native app:',target/WINDOWS/'release/oh-my-3do-library.exe')

if args.windows and os.name!='nt':cross_windows();raise SystemExit
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
