#!/usr/bin/env python3
"""Build the Tauri desktop app and its shared Rust engine; no Node or Python runtime is shipped."""
import argparse,json,os,shutil,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
WINDOWS='x86_64-pc-windows-msvc'
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--target-dir',type=Path);p.add_argument('--cargo');p.add_argument('--test',action='store_true')
p.add_argument('--windows',action='store_true',help='cross-compile Windows x64 with cargo-xwin in Docker')
p.add_argument('--legacy-ui',action='store_true',help='build the previous egui interface for development comparisons')
args=p.parse_args()
subprocess.run([os.environ.get('PYTHON','python3'),str(ROOT/'tools/build-save-cheats.py'),'--check'],check=True)
manifest=ROOT/'desktop'/('Cargo.toml' if args.legacy_ui else 'shell/Cargo.toml')
features=['--features','legacy-ui'] if args.legacy_ui else []
def run_tests(command,common,core_manifest):
    suites=[core_manifest] if args.legacy_ui else [core_manifest,Path(core_manifest).parent/'shell/Cargo.toml']
    for suite in suites:
        subprocess.run(command+['test','--manifest-path',str(suite)]+common,check=True)
def docker_build(image,dockerfile):
    subprocess.run(['docker','build','-t',image,'-f',str(ROOT/'tools'/dockerfile),str(ROOT)],check=True)
def container(windows):
    if not shutil.which('docker'):raise SystemExit('Install Docker, or install Tauri build prerequisites and a Rust toolchain locally')
    image='oh3do-desktop-windows' if windows else 'oh3do-desktop-tauri'
    docker_build(image,'desktop-windows.Dockerfile' if windows else 'desktop-tauri.Dockerfile')
    target=(args.target_dir or ROOT/('build/windows-target' if windows else 'build/tauri-linux-target')).resolve()
    cache=ROOT/'build/xwin-cache';target.mkdir(parents=True,exist_ok=True);cache.mkdir(parents=True,exist_ok=True)
    flags=['--remap-path-prefix=/src=oh-my-3do','--remap-path-prefix=/cache/cargo=cargo']
    if windows:flags[:0]=['-C','target-feature=+crt-static']
    run=['docker','run','--rm','--user',f'{os.getuid()}:{os.getgid()}','-v',f'{ROOT}:/src','-v',f'{target}:/target','-v',f'{cache}:/cache','-w','/src',
         '-e','HOME=/cache','-e','CARGO_HOME=/cache/cargo','-e','CARGO_BUILD_JOBS=3','-e','XWIN_CACHE_DIR=/cache/xwin','-e','XWIN_ACCEPT_LICENSE=1',
         '-e','CARGO_ENCODED_RUSTFLAGS='+'\x1f'.join(flags),image,'cargo']+(['xwin'] if windows else [])
    common=['--locked','--target-dir','/target']+(['--target',WINDOWS] if windows else [])
    if args.test:
        for suite in ['/src/desktop/Cargo.toml']+([] if args.legacy_ui else ['/src/desktop/shell/Cargo.toml']):
            command=run+['test','--manifest-path',suite]+common
            if windows:
                out=subprocess.run(command+['--no-run','--message-format=json-render-diagnostics'],check=True,stdout=subprocess.PIPE,text=True).stdout
                tests=[target/Path(m['executable']).relative_to('/target') for m in map(json.loads,out.splitlines()) if m.get('reason')=='compiler-artifact' and m.get('executable') and m['profile']['test']]
                if Path('/proc/sys/fs/binfmt_misc/WSLInterop').exists():
                    for test in tests:subprocess.run([str(test)],check=True)
                else:print('Run Windows test executables on Windows:',*tests,sep='\n  ')
            else:subprocess.run(command,check=True)
    relative='/src/'+manifest.relative_to(ROOT).as_posix()
    subprocess.run(run+['build','--release','--manifest-path',relative]+common+features,check=True)
    print('Desktop app:',target/(WINDOWS if windows else '')/'release'/('oh-my-3do-library.exe' if windows else 'oh-my-3do-library'))
if args.windows and os.name!='nt':container(True);raise SystemExit
if os.name=='posix' and not args.legacy_ui and subprocess.run(['pkg-config','--exists','webkit2gtk-4.1','gtk+-3.0']).returncode:
    container(False);raise SystemExit
cargo=args.cargo or (str(Path.home()/'.cargo/bin/cargo.exe') if os.name=='nt' else shutil.which('cargo'))
if not cargo:raise SystemExit('Install the stable Rust toolchain first')
target=(args.target_dir or (Path(tempfile.gettempdir())/'oh3do-native-build' if os.name=='nt' else ROOT/'build/native-target')).resolve()
env=os.environ.copy();flags=['--remap-path-prefix='+str(ROOT)+'=oh-my-3do','--remap-path-prefix='+str(Path.home()/'.cargo')+'=cargo']
if os.name=='nt':flags[:0]=['-C','target-feature=+crt-static']
env['CARGO_ENCODED_RUSTFLAGS']='\x1f'.join(flags)
if args.test:
    for suite in [ROOT/'desktop/Cargo.toml']+([] if args.legacy_ui else [manifest]):subprocess.run([cargo,'test','--locked','--manifest-path',str(suite),'--target-dir',str(target)],cwd=ROOT,env=env,check=True)
subprocess.run([cargo,'build','--release','--locked','--manifest-path',str(manifest),'--target-dir',str(target)]+features,cwd=ROOT,env=env,check=True)
print('Desktop app:',target/'release'/('oh-my-3do-library.exe' if os.name=='nt' else 'oh-my-3do-library'))
