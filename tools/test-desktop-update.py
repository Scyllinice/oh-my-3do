#!/usr/bin/env python3
"""Exercise Tauri executable replacement, frontend-ready acknowledgement and rollback in disposable directories. Run under Xvfb/D-Bus."""
import os,time,json,subprocess,shutil,hashlib
from pathlib import Path
R=Path(__file__).resolve().parents[1];base=R/'build/desktop-update-smoke';binary=R/'build/tauri-linux-target/release/oh-my-3do-library'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def cleanup(exe):
 for p in Path('/proc').iterdir():
  if not p.name.isdigit():continue
  try:
   command=(p/'cmdline').read_bytes().split(b'\0')[0]
   if command==str(exe).encode():os.kill(int(p.name),15)
  except (OSError,ValueError):pass
for fail in [False,True]:
 work=base/('rollback' if fail else 'success');work.mkdir(parents=True,exist_ok=True);exe=work/'OhMy3DOLibrary';stage=work/'staged';previous=work/'previous';ready=work/'ready';log=work/'install.log';support=work/'asset-readme';readme=work/'README.md'
 for p in [exe,stage,previous,ready,log]:
  if p.exists():p.unlink()
 shutil.copy2(binary,exe)
 with exe.open('ab') as f:f.write(b'ORIGINAL VERSION')
 original=sha(exe);shutil.copy2(binary,stage)
 if fail:stage.write_bytes(b'\x7fELF'+bytes(100));stage.chmod(0o755)
 support.write_text('Updated release documentation');readme.write_text('Original release documentation');(work/'native-settings.json').write_text('Personal preferences must stay unchanged')
 cache=work/'cache/oh-my-3do';cache.mkdir(parents=True,exist_ok=True);(cache/'native-settings.json').write_text(json.dumps(dict(offline=True,disable_update_checks=True)))
 plan=dict(executable=str(exe),staged=str(stage),previous=str(previous),ready=str(ready),log=str(log),original_sha256=original,new_sha256=sha(stage),parent=99999999,assets=[dict(relative='README.md',staged=str(support),sha256=sha(support))]);path=work/'install.json';path.write_text(json.dumps(plan))
 env=os.environ.copy();env.update(XDG_CACHE_HOME=str(work/'cache'),XDG_CONFIG_HOME=str(work/'config'),XDG_DATA_HOME=str(work/'data'),WEBKIT_DISABLE_DMABUF_RENDERER='1',LIBGL_ALWAYS_SOFTWARE='1')
 helper=subprocess.Popen([str(binary),'--apply-desktop-update',str(path)],env=env,stdout=open(work/'process.log','w'),stderr=subprocess.STDOUT)
 try:
  result=helper.wait(timeout=60)
  if fail:
   assert result!=0;assert sha(exe)==original;assert readme.read_text()=='Original release documentation';assert not previous.exists();print('Updater failure: launch error restored original executable and support files.',flush=True)
  else:
   assert result==0,(result,(work/'process.log').read_text());assert ready.exists();assert sha(exe)==plan['new_sha256'];assert sha(previous)==original;assert readme.read_text()=='Updated release documentation';print('Updater success: replaced executable and docs, relaunched actual Tauri UI, received frontend-ready acknowledgement; previous executable retained.',flush=True)
  assert (work/'native-settings.json').read_text()=='Personal preferences must stay unchanged'
 finally:
  cleanup(exe)
  if helper.poll() is None:helper.kill();helper.wait()
