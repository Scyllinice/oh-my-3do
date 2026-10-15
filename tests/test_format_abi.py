#!/usr/bin/env python3
"""Check the actual menu formatter against Norcroft/Portfolio's argument ABI.

Run in the development container. Host libc uses its own ABI and cannot detect
this bug. The original built-in-header version must fail the char* contract;
the production SDK-header version must compile with that same contract.
"""
from pathlib import Path
import os
import re
import subprocess

root = Path(__file__).resolve().parent.parent
source = (root / 'src/main.c').read_text()
formatter = source[source.index('static void format('):source.index('static unsigned long now_ms(')]
headers = '\n'.join(re.findall(r'^#include ["<](?:stdio|stdarg)\.h[">]$', source, re.M))
output = root / 'build/format-abi'
output.mkdir(parents=True, exist_ok=True)
flags = ['armcc', '-I' + os.environ['TDO_DEVKIT_PATH'] + '/include/3do',
         '-DOH3DO_SDK=1', '-O2', '-bigend', '-za1', '-zi4', '-fpu', 'none', '-arch', '3',
         '-apcs', '3/32/nofp/swst/wide/softfp']
for name, includes, success in (
    ('portfolio', headers, True),
    ('original-builtin', '#include <stdio.h>\n#include <stdarg.h>', False),
):
    path = output / (name + '.c')
    path.write_text(includes + '\n' + formatter + '\nvoid probe(char *b) { format(b, "%lu %u", 0UL, 16U); }\n')
    result = subprocess.run(flags + ['-S', str(path), '-o', str(path.with_suffix('.s'))],
                            capture_output=True, text=True)
    (output / (name + '.log')).write_text(result.stdout + result.stderr)
    if (result.returncode == 0) != success:
        raise RuntimeError(name + ': unexpected compiler result\n' + result.stdout + result.stderr)
    if not success and 'pointer to non-equal pointer' not in result.stderr + result.stdout:
        raise RuntimeError('Original headers failed for a reason other than the va_list ABI')
print('Formatter ABI: production compiles; original built-in-header mismatch is rejected')
