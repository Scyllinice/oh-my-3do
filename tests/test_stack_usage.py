#!/usr/bin/env python3
"""Bound actual ARM stack allocations on the 32 KiB menu launch path."""
import os
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parent.parent
sdk = Path(os.environ['TDO_DEVKIT_PATH'])
out = root / 'build/stack-audit'
out.mkdir(parents=True, exist_ok=True)
assembly = out / 'main.s'
result = subprocess.run([
    'armcc', '-I' + str(sdk / 'include/3do'), '-I' + str(sdk / 'include/community'),
    '-DOH3DO_SDK=1', '-DNDEBUG=1', '-O2', '-bigend', '-za1', '-zi4',
    '-fpu', 'none', '-arch', '3', '-apcs', '3/32/nofp/swst/wide/softfp',
    '-S', str(root / 'src/main.c'), '-o', str(assembly)
], capture_output=True, text=True)
(out / 'compile.log').write_text(result.stdout + result.stderr)
result.check_returncode()
text = assembly.read_text()
# Saved registers add at most 64 bytes per function. Prevent activate's frame
# from overlapping offer_group's existing large buffers on a single call chain.
limits = {'activate': 512, 'offer_group': 20000, 'remember_position': 512,
          'record_launch': 512, 'export_diagnostics': 8192}
summary = []
for name, limit in limits.items():
    body = text.split('\n' + name + '\n', 1)[1]
    # Norcroft reserves locals before the first ordinary call (BLCC is its
    # stack-limit guard). Inspect generated instructions, not C declarations.
    prologue = re.split(r'\n\s+BL\s+', body, maxsplit=1)[0]
    size = sum(int(n, 0) for n in re.findall(r'^\s+SUB\s+sp,sp,#(0x[0-9a-fA-F]+|\d+)\s*$', prologue, re.M))
    assert size <= limit, f'{name}: {size} bytes exceeds {limit}-byte local-stack budget'
    summary.append(f'{name}: {size} bytes of explicit local stack')
(out / 'frames.txt').write_text('\n'.join(summary) + '\n')
print('ARM stack: small launch/persistence frames; disc grouping and diagnostics fit their budgets')
