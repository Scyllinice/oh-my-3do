#!/usr/bin/env python3
"""Package a prebuilt native GUI with exact dependency notices; not a runtime dependency."""
import argparse,base64,hashlib,json,os,re,shutil,subprocess,zipfile,urllib.request,urllib.error
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('binary',type=Path);p.add_argument('--platform',choices=['Windows-x64','Linux-x64','macOS-arm64','macOS-x64'],required=True)
p.add_argument('--revision',default='working-tree');p.add_argument('--cargo',default='cargo');args=p.parse_args()
assert args.binary.is_file()
version=re.search(r'^version\s*=\s*"([^"]+)"',(ROOT/'desktop/shell/Cargo.toml').read_text(),re.M).group(1);folder=ROOT/'build'/('desktop-'+args.platform)
if folder.exists():shutil.rmtree(folder)
folder.mkdir(parents=True)
name='OhMy3DOLibrary'+('.exe' if args.platform.startswith('Windows') else '')
shutil.copy2(args.binary,folder/name)
shutil.copy2(ROOT/'LICENSE',folder/'LICENSE')
shutil.copy2(ROOT/'desktop/README.md',folder/'README.md')
shutil.copy2(ROOT/'desktop/THIRD_PARTY.md',folder/'THIRD_PARTY.md')
shutil.copy2(ROOT/'assets/app-icon.png',folder/'OhMy3DOLibrary.png')
(folder/'licenses'/'fonts').mkdir(parents=True)
for font_license in ('ark-pixel-OFL.txt','atkinson-hyperlegible-OFL.txt'):shutil.copy2(ROOT/'vendor/fonts'/font_license,folder/'licenses'/'fonts'/font_license)
triple={'Windows-x64':'x86_64-pc-windows-msvc','Linux-x64':'x86_64-unknown-linux-gnu','macOS-arm64':'aarch64-apple-darwin','macOS-x64':'x86_64-apple-darwin'}[args.platform]
meta=json.loads(subprocess.run([args.cargo,'metadata','--manifest-path',str(ROOT/'desktop/shell/Cargo.toml'),'--format-version','1','--locked','--filter-platform',triple],check=True,capture_output=True,text=True,encoding='utf-8').stdout)
used={node['id'] for node in meta['resolve']['nodes']}
notices=[];missing=[]
for package in meta['packages']:
 if package['name'] in ('oh-my-3do-library','oh-my-3do-desktop') or package['id'] not in used:continue
 manifest=Path(package['manifest_path']);source=manifest.parent
 target=folder/'licenses'/(package['name']+'-'+package['version'])
 texts=[]
 if source.is_dir():
  for path in source.rglob('*'):
   if not path.is_file():continue
   relative=path.relative_to(source)
   if len(relative.parts)>4:continue
   label=path.name.lower()
   if ((any(key in label for key in ('license','licence','copyright','copying','notice')) or ('fonts' in relative.parts and path.suffix.lower() in ('.txt','.md'))) and path.suffix.lower() not in ('.rs','.c','.h','.ttf','.woff','.woff2','.png','.jpg','.svg')):
    dest=target/relative;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,dest);texts.append(relative.as_posix())
 if package.get('license_file'):
  src=source/package['license_file']
  if src.is_file() and src.name not in texts:
   target.mkdir(parents=True,exist_ok=True);shutil.copy2(src,target/src.name);texts.append(src.name)
 if not texts:
  # Some published crates omit their license files. Keep pinned upstream notices
  # beside the app sources rather than requiring a mutable network fallback.
  supplied=ROOT/'desktop/licenses'/(package['name']+'-'+package['version'])
  if supplied.is_dir():
   for path in sorted(supplied.iterdir()):
    if path.is_file():
     target.mkdir(parents=True,exist_ok=True);shutil.copy2(path,target/path.name);texts.append(path.name)
 if not texts:
  # Some workspace crates omit inherited license files from the crate tarball.
  # Retrieve the exact repository revision recorded by Cargo at publication.
  info=source/'.cargo_vcs_info.json';repo=package.get('repository') or ''
  match=re.match(r'https://github.com/([^/]+)/([^/#]+)',repo)
  if info.is_file() and match:
   vcs=json.loads(info.read_text(encoding='utf-8'));revision=vcs.get('git',{}).get('sha1','')
   if re.fullmatch('[0-9a-f]{40}',revision):
    cache=ROOT/'build/license-cache'/('-'.join(match.groups())+'-'+revision+'.json');cache.parent.mkdir(parents=True,exist_ok=True)
    try:
     if cache.exists():license=json.loads(cache.read_text(encoding='utf-8'))
     else:
      url='https://api.github.com/repos/'+match[1]+'/'+match[2]+'/license?ref='+revision
      request=urllib.request.Request(url,headers={'User-Agent':'OhMy3DO-Desktop-Packager'})
      with urllib.request.urlopen(request,timeout=30) as response:license=json.load(response)
      cache.write_text(json.dumps(license),encoding='utf-8')
     text=base64.b64decode(license['content']);target.mkdir(parents=True,exist_ok=True)
     filename='upstream-'+Path(license['path']).name
     (target/filename).write_bytes(text);texts.append(filename)
    except (OSError,ValueError,KeyError) as error:print('License lookup:',package['name'],error)
 if not texts and package.get('license')=='CC0-1.0':
  cache=ROOT/'build/license-cache/CC0-1.0.txt';cache.parent.mkdir(parents=True,exist_ok=True)
  if not cache.exists():
   request=urllib.request.Request('https://raw.githubusercontent.com/spdx/license-list-data/main/text/CC0-1.0.txt',headers={'User-Agent':'OhMy3DO-Desktop-Packager'})
   with urllib.request.urlopen(request,timeout=30) as response:cache.write_bytes(response.read())
  target.mkdir(parents=True,exist_ok=True);shutil.copy2(cache,target/'CC0-1.0.txt');texts.append('CC0-1.0.txt')
 if not texts:missing.append(package['name']+'-'+package['version'])
 notices.append({'name':package['name'],'version':package['version'],'license':package.get('license'),'repository':package.get('repository'),'notices':sorted(set(texts))})
(folder/'licenses').mkdir(exist_ok=True)
(folder/'licenses/dependencies.json').write_text(json.dumps(notices,indent=2),encoding='utf-8')
info={'app_version':version,'platform':args.platform,'revision':args.revision,'binary_sha256':hashlib.sha256((folder/name).read_bytes()).hexdigest(),'cargo_lock_sha256':hashlib.sha256((ROOT/'desktop/shell/Cargo.lock').read_bytes()).hexdigest(),'runtime':'Tauri 2 + Rust; system WebView; no Node/Python runtime; FFmpeg downloaded separately','dependencies_without_separate_notice_files':missing}
if args.platform=='Linux-x64' and shutil.which('objdump'):
 output=subprocess.run(['objdump','-T',str(args.binary)],check=True,capture_output=True,text=True).stdout
 versions=re.findall(r'GLIBC_([0-9.]+)',output)
 if versions:info['minimum_glibc']=max(versions,key=lambda v:tuple(map(int,v.split('.'))))
(folder/'BUILD.json').write_text(json.dumps(info,indent=2),encoding='utf-8')
if missing:raise SystemExit('Dependency notices are incomplete: '+', '.join(missing))
archive=ROOT/'iso'/f'OhMy3DO-Library-{args.platform}-v{version}.zip';archive.parent.mkdir(exist_ok=True)
with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,compresslevel=9,strict_timestamps=False) as z:
 for path in sorted(folder.rglob('*')):
  if path.is_file():z.write(path,'OhMy3DO-Library/'+path.relative_to(folder).as_posix())
sha=hashlib.sha256(archive.read_bytes()).hexdigest()
archive.with_suffix('.zip.sha256').write_text(sha+'  '+archive.name+'\n',encoding='utf-8')
subprocess.run([os.sys.executable,str(ROOT/'tools/write-update-manifest.py'),'desktop','--artifact',str(archive),'--platform',args.platform,'--version',version],check=True)
print('Packaged:',archive)
print('Dependency notices:',len(notices),'packages; without separate notice files:',', '.join(missing))
