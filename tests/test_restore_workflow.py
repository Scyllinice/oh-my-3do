#!/usr/bin/env python3
"""Exercise the actual restore controller with failures before every stage.

The real NVRAM/USB implementations have separate tests; here callbacks record
ordering and establish that no NVRAM write precedes verified recovery storage.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
source = (root / 'src/main.c').read_text()
controller = source[source.index('static void restore_selected('):source.index('static void tools_input(Menu *menu, u32 pressed)\n{')]
stubs = r'''
#include "nvram_io.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct Menu {
    SaveArchive backup;
    size_t save_selected;
    unsigned int backup_slot;
    NvramInfo nvram_info;
    int restore_result, usb_tested;
    const char *tool_message;
} Menu;
static int stage, fail_stage, writes, recovered;
static const char *recovery_path;
static int step(void) { return ++stage == fail_stage; }
static void draw(Menu *m) { (void)m; }
static int test_save_usb(Menu *m) { (void)m; return step() ? -1 : 0; }
int nvram_preflight(const SaveEntry *entry, NvramInfo *info) {
    (void)entry; info->error="preflight failure"; return step() ? -1 : 0;
}
static int fixture(SaveArchive *a, int restored) {
    SaveEntry e[2]; memset(e,0,sizeof(e));
    strcpy(e[0].name,"Game"); e[0].size=3; e[0].type=0x33444f46UL;
    e[0].data=(const unsigned char *)(restored ? "new" : "old");
    strcpy(e[1].name,"Other"); e[1].size=4; e[1].data=(const unsigned char *)"keep";
    return save_archive_build(a,e,2);
}
int nvram_snapshot(SaveArchive *a, NvramInfo *info) {
    info->error="snapshot failure";
    if (step()) return -1;
    return fixture(a,writes);
}
static int save_root_operation(Menu *m, int action, const char *path, SaveArchive *a) {
    (void)m; assert(action == 1 && !writes && a->count == 2);
    assert(!memcmp(a->entries[0].data,"old",3));
    recovery_path=path;
    if (step()) return -1;
    recovered=1; return 0;
}
int nvram_restore(const SaveEntry *entry, NvramInfo *info) {
    assert(recovered); ++writes;
    assert(!memcmp(entry->data,"new",3)); info->error="restore failure";
    return step() ? -1 : 0;
}
'''
main = r'''
int main(void) {
    Menu m; int fail, limit;
    memset(&m,0,sizeof(m)); fixture(&m.backup,1);
    restore_selected(&m); assert(writes == 1 && recovered && m.restore_result == 1);
    assert(!strcmp(recovery_path,"saves/recovery.3ds")); limit=stage;
    for (fail=1;fail<=limit;++fail) {
        stage=writes=recovered=0; fail_stage=fail; m.usb_tested=0;
        restore_selected(&m);
        assert(m.restore_result == -1);
        if (fail <= 4) assert(!writes); /* test, preflight, capture, recovery */
        if (writes) assert(recovered);
    }
    stage=writes=recovered=fail_stage=0; m.backup_slot=SAVE_RECOVERY_SLOT;
    restore_selected(&m); assert(m.restore_result == 1 && !strcmp(recovery_path,"saves/recovery2.3ds"));
    save_archive_free(&m.backup);
    puts("Restore workflow: no NVRAM writes before verified recovery; all stage failures and recovery-source preservation passed");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='oh3do-restore-') as directory:
    code, binary = Path(directory)/'test.c', Path(directory)/'test'
    code.write_text(stubs + controller + main)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c89', '-Wall', '-Wextra', '-Werror', '-Isrc', str(code),
                    'src/save_archive.c', 'src/usb_art.c', 'src/menu_ui.c', 'src/menu_categories.c', 'src/cover_catalog.c', '-o', str(binary)], cwd=root, check=True)
    subprocess.run([str(binary)], check=True)
