#!/usr/bin/env python3
"""Check complete runtime preservation and reject missing/modified boot files."""
import os
from pathlib import Path
import runpy
import shutil
import tempfile

root = Path(__file__).resolve().parent.parent
sdk = Path(os.environ['TDO_DEVKIT_PATH']) / 'takeme'
module = runpy.run_path(str(root / 'tools/prepare-runtime.py'))
prepare, template_files = module['prepare'], module['template_files']

with tempfile.TemporaryDirectory(prefix='oh3do-runtime-') as directory:
    stage, audit = Path(directory) / 'disc', Path(directory) / 'audit.json'
    stage.mkdir()
    for source in template_files(sdk):
        target = stage / source.relative_to(sdk)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    before = {path.relative_to(stage): path.read_bytes()
              for path in stage.rglob('*') if path.is_file()}
    prepare(stage, audit)
    after = {path.relative_to(stage): path.read_bytes()
             for path in stage.rglob('*') if path.is_file()}
    assert before == after, 'Runtime preparation must not change any SDK file'
    for name in ('System/Audio/dsp/head.dsp', 'System/Scripts/startopera',
                 'System/Folios/audio.privfolio', 'System/Tasks/eventbroker'):
        target = stage / name
        original = target.read_bytes()
        for changed in (None, b'corrupt runtime file'):
            if changed is None:
                target.unlink()
            else:
                target.write_bytes(changed)
            try:
                prepare(stage, audit)
            except ValueError:
                pass
            else:
                raise AssertionError('Accepted missing/modified ' + name)
            target.write_bytes(original)
    prepare(stage, audit)
print('Runtime: complete SDK template preserved; missing/modified boot dependencies rejected')
