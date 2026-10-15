#!/usr/bin/env python3
"""Check the actual SDK staging operation and preserve boot dependencies."""
import os
from pathlib import Path
import runpy
import shutil
import tempfile

root = Path(__file__).resolve().parent.parent
sdk = Path(os.environ['TDO_DEVKIT_PATH']) / 'takeme'
prepare = runpy.run_path(str(root / 'tools/prepare-runtime.py'))['prepare']


def commands(path):
    return [line.strip() for line in path.read_text().splitlines()
            if line.strip() and not line.lstrip().startswith('#')]


# Preserve the SDK's startup behavior, including aliases used by services.
for name in ('AppStartup', 'System/Scripts/startopera'):
    assert commands(root / 'runtime' / name) == commands(sdk / name), name

with tempfile.TemporaryDirectory(prefix='oh3do-runtime-') as directory:
    stage = Path(directory) / 'disc'
    shutil.copytree(sdk, stage)
    prepare(stage, Path(directory) / 'audit.json')
    # These are operating-system startup dependencies, even for a silent menu.
    required = ['System/Folios/' + name + '.privfolio'
                for name in ('graphics', 'operamath', 'audio')]
    required += ['System/Audio/dsp/' + name + '.dsp'
                 for name in ('romhead', 'romtail', 'tail', 'oscupdownfp', 'splitexec')]
    required += ['System/Drivers/' + name + '.rom'
                 for name in ('cport1', 'cport41', 'cport49', 'cport4d')]
    required += ['System/Tasks/shell', 'System/Tasks/eventbroker',
                 'System/Programs/fscheck', 'System/Programs/lmadm', 'System/Programs/format']
    for name in required:
        assert (stage / name).read_bytes() == (sdk / name).read_bytes(), name
    assert not (stage / 'System/Audio/aiff/sinewave.aiff').exists()
    assert not (stage / 'System/Folios/example.privfolio').exists()
    for name in ('AppStartup', 'System/Scripts/startopera'):
        assert commands(stage / name) == commands(sdk / name), name
print('Runtime: SDK startup commands and OS audio/controller dependencies preserved')
