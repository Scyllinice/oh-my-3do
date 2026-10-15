#!/usr/bin/env python3
"""Exercise the menu's real format wrapper with representative display values."""
from pathlib import Path
import os
import subprocess
import tempfile
root = Path(__file__).resolve().parent.parent
source = (root / 'src/main.c').read_text()
formatter = source[source.index('static void format('):source.index('static unsigned long now_ms(')]
checks = r'''
#include <assert.h>
#include <string.h>
int main(void) {
    char out[80];
    char number[4] = {'x','x','x','x'};
    unsigned long n;
    for (n=0;n<=16;++n) {
        char expected[80];
        format(out,"%2lu / 16 DISCS",n);
        sprintf(expected,"%2lu / 16 DISCS",n);
        assert(!strcmp(out,expected));
        format(number,"%2lu",n); assert(strlen(number) == 2 && number[3] == 'x');
    }
    format(out,"%lu files %lu used %lu free",27UL,16040UL,16408UL);
    assert(!strcmp(out,"27 files 16040 used 16408 free"));
    format(out,"Reported revision: %u.%u.%u",1U,1U,0U);
    assert(!strcmp(out,"Reported revision: 1.1.0"));
    format(out,"%02x %02x %02x %02x %02x %02x",0x93U,1U,0x4cU,0x4fU,0x43U,0x4fU);
    assert(!strcmp(out,"93 01 4c 4f 43 4f"));
    format(out,"saves/slot%02u.3ds",8U); assert(!strcmp(out,"saves/slot08.3ds"));
    format(out,"Config result: %d (0 = loaded)",-1);
    assert(!strcmp(out,"Config result: -1 (0 = loaded)"));
    format(out,"XBUS error: %08lx",0x80000001UL); assert(!strcmp(out,"XBUS error: 80000001"));
    format(out,"ODE ID: %.6s","LOCODE trailing"); assert(!strcmp(out,"ODE ID: LOCODE"));
    puts("Formatter values: queue counts, save sizes, identity, errors and slot paths passed");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='oh3do-format-') as directory:
    path, binary = Path(directory)/'test.c', Path(directory)/'test'
    path.write_text('#include <stdio.h>\n#include <stdarg.h>\n' + formatter + checks)
    subprocess.run([os.environ.get('CC','cc'),'-std=c89','-Wall','-Wextra','-Werror',str(path),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
