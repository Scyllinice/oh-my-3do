#!/usr/bin/env python3
"""Exercise the production recovery controller, including its no-I/O gates."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
source = (root / "src/main.c").read_text()
controller = source[
    source.index("static int can_recover_library(") : source.index(
        "static void refresh_usb_art(Menu *menu)\n{"
    )
]
stubs = r"""
#include "ode.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct Menu {
    Ode ode; OdeListing listing;
    struct { int timed_out; } bus;
    int root_known,demo,recovery_result,recovery_media_off,covers_enabled;
    int ready,view_valid,tools,review,alphabet_active,clear_frames,position_dirty;
    unsigned long root_fingerprint,root_volume;
    unsigned int depth; size_t selected; char filter[32];
    int cover_cache; const char *message,*tool_message;
} Menu;
static int calls,fail_core,stopped,closed,cleared,changed,drawn;
static MenuEntry root_entry={1,1,"Games"};
static const MenuEntry *root_entries[]={&root_entry};
void ode_listing_init(OdeListing *l) { memset(l,0,sizeof(*l)); }
int ode_recover_root(Ode *o,unsigned long hash,unsigned long volume,OdeListing *out) {
    assert(hash==123);assert(volume==0 || volume==0x1000000UL);++calls;
    if(fail_core) return -1;
    out->entries=root_entries;out->count=1;o->navigation_failed=0;return 0;
}
static void stop_preview(Menu *m) { (void)m;++stopped; }
static void close_full_cover(Menu *m) { (void)m;++closed; }
static void draw(Menu *m) { assert(!m->ready && !m->view_valid);++drawn; }
static void free_listing(Menu *m) { memset(&m->listing,0,sizeof(m->listing)); }
static void cover_cache_clear(int *c) { (void)c;++cleared; }
static void update_breadcrumb(Menu *m) { assert(m->depth<=1); }
static void selection_changed(Menu *m) { m->position_dirty=1;++changed; }
static void notify(Menu *m,const char *s) { m->message=s; }
static void setup(Menu *m) {
    memset(m,0,sizeof(*m));strcpy(m->ode.identity,"LOCODE");
    m->root_known=1;m->root_fingerprint=123;m->depth=3;m->selected=7;
    m->ready=m->view_valid=m->tools=m->review=m->alphabet_active=1;
    m->covers_enabled=1;m->ode.navigation_failed=1;m->ode.media_beside=1;
    m->ode.media_folder="Games/Child";strcpy(m->filter,"old");
}
"""
checks = r"""
int main(void) {
    Menu m; int i,prior;
    for(i=0;i<6;++i) {
        setup(&m);
        switch(i) {
        case 0:m.bus.timed_out=1;break;
        case 1:m.ode.transport_uncertain=1;break;
        case 2:strcpy(m.ode.identity,"OTHER");break;
        case 3:m.root_known=0;break;
        case 4:m.demo=1;break;
        default:m.recovery_result=-1;break;
        }
        prior=calls;assert(recover_library(&m)<0 && calls==prior);
        assert(m.ode.navigation_failed && m.depth==3 && m.selected==7);
    }
    for(i=0;i<2;++i) {
        setup(&m);if(i) m.root_volume=0x1000000UL;
        assert(recover_library(&m)==0);
        assert(m.listing.entries==root_entries && m.listing.count==1);
        assert(m.depth==(unsigned int)i && !m.selected && !m.filter[0]);
        assert(m.ready && m.view_valid && !m.tools && !m.review && !m.alphabet_active);
        assert(!m.ode.navigation_failed && m.recovery_result==1);
        assert(m.recovery_media_off && !m.covers_enabled && !m.position_dirty);
        assert(!m.ode.media_folder && !m.ode.media_beside && m.clear_frames==2);
    }
    setup(&m);fail_core=1;
    assert(recover_library(&m)<0 && m.recovery_result==-1);
    assert(m.ode.navigation_failed && !m.ready && !m.view_valid);
    assert(m.depth==3 && m.selected==7 && !m.listing.entries);
    prior=calls;assert(recover_library(&m)<0 && calls==prior);
    assert(stopped==3 && closed==3 && drawn==3 && cleared==2 && changed==2);
    puts("Recovery controller: no-I/O gates, single/multi-volume adoption, media suppression and failed retry passed");
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="oh3do-recovery-") as directory:
    code, binary = Path(directory) / "test.c", Path(directory) / "test"
    code.write_text(stubs + controller + checks)
    subprocess.run(
        [
            os.environ.get("CC", "cc"),
            "-std=c89",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Isrc",
            str(code),
            "-o",
            str(binary),
        ],
        cwd=root,
        check=True,
    )
    subprocess.run([str(binary)], check=True)
