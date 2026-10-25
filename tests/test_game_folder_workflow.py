#!/usr/bin/env python3
"""Run the menu's game-folder launch and position code without entering its UI."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
source = (root / 'src/main.c').read_text()

def section(start, end):
    return source[source.index(start):source.index(end)]

stubs = r'''
#include "library_index.h"
#include "library_features.h"
#include "menu_position.h"
#include "menu_config.h"
#include "menu_ui.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define MAX_DEPTH 32
/*GAME_LOOKUP_TYPES*/
typedef struct Menu {
    Ode ode; OdeListing listing; OdePlaylist playlist; LibraryMetadata metadata;
    MenuPosition last_position; MenuConfig config;
    unsigned int depth; unsigned long path_ids[MAX_DEPTH];
    char path[MAX_DEPTH][ODE_MAX_NAME]; size_t previous_selection[MAX_DEPTH],selected;
    int ready,view_valid,demo,loading,review,tools,single_disc,launching;
    int config_result,position_result,position_dirty,recovery_media_off;
    unsigned long selection_since,listing_generation;
/*GAME_LOOKUP_FIELDS*/
    struct { int timed_out; } bus;
    char status[128]; const char *message;
    void *preview_cel;
} Menu;
static int entered,loaded,mounted,applied,fail_apply,fail_mount,stopped,writes;
static char saved[POSITION_BYTES],recent[9218];
static size_t row_count(Menu *m) { return m->listing.count+(m->listing.has_parent ? 1 : 0); }
static const MenuEntry *browser_entry(Menu *m,size_t n) { return m->listing.entries[n]; }
static int hidden_entry(Menu *m,const MenuEntry *e) { (void)m;(void)e;return 0; }
static int folder_workaround(Menu *m) { (void)m;return 1; }
int library_path_safe(const char *p) { return !strstr(p,".."); }
/* The harness borrows its listing, so launching drops it rather than freeing it. */
static void free_listing(Menu *m) { m->listing.entries=NULL;m->listing.count=0; }
static void go_back(Menu *m) { (void)m;assert(0); }
static void draw(Menu *m) { (void)m; }
static void stop_preview(Menu *m) { (void)m;++stopped; }
static void notify(Menu *m,const char *s) { m->message=s; }
#define format sprintf
static unsigned long now_ms(Menu *m) { (void)m;return 5000; }
static void load_directory(Menu *m,size_t n) { (void)m;(void)n;++loaded; }
#define ode_enter_directory test_enter_directory
int ode_enter_directory(Ode *o,const OdeListing *l,const MenuEntry *e,int w) {
    (void)o;(void)l;(void)e;(void)w;++entered;return 0;
}
static int queue_set(Menu *m,const MenuEntry *e) { (void)m;(void)e;return 0; }
static int apply_queue(Menu *m) {
    ++applied;
    assert(m->depth==2 && m->selected==1);
    assert(!strcmp(m->path[1],"USA"));
    if (fail_apply) return -1;
    m->playlist.dirty=0;return 0;
}
static int text_at_root(Menu *m,const char *name,const char *write,char *read,size_t cap,size_t *n) {
    (void)m;(void)read;(void)cap;(void)n;
    assert(!strcmp(name,"last-position.txt"));strcpy(saved,write);++writes;return 0;
}
static void remember_position(Menu *m,int force);
static void record_launch(Menu *m,const char *path) {
    remember_position(m,1);strcpy(recent,path);
}
static void start_queue(Menu *m);
static void queue_selected_disc(Menu *m);
static int transfer(void *ctx,const unsigned char *p,unsigned char *r,size_t cap,size_t *n) {
    (void)ctx;memset(r,0,cap);r[0]=p[0];
    assert(p[0]==0xc5);++mounted;*n=2;return fail_mount ? -1 : 0;
}
'''

stubs=stubs.replace('/*GAME_LOOKUP_TYPES*/',section('#define GAME_LOOKUP_SLOTS', '#define SCROLL_MEASURE_SLOTS'))
stubs=stubs.replace('/*GAME_LOOKUP_FIELDS*/',section('    GameLookup game_lookups[GAME_LOOKUP_SLOTS];','    ScrollMeasure scroll_measures'))

controller = ''.join([
    section('static int directory_path(Menu *menu, char *buffer, size_t capacity)\n{', 'typedef struct UsbCoverRead'),
    section('static long relative_folder(Menu *menu,char *path,size_t size,size_t room)\n{', "/* Resolve the selected game's metadata once"),
    section('static void activate(Menu *menu)\n{', 'static void review_queue(Menu *menu)\n{'),
    section('static void start_queue(Menu *menu)\n{', '/* Tools stay available after a transport failure'),
    section('static void remember_position(Menu *menu, int force)\n{', 'typedef struct RecentLaunch'),
])

checks = r'''
int main(void) {
    static LibraryGameInfo games[]={
        {"Games/USA/One/One.iso","","",""},
        {"Games/USA/Set/Set (Disc 1).iso","","",""},
        {"Games/USA/Set/Set (Disc 2).iso","","",""}
    };
    const MenuEntry one={1,1,"One"},set={2,1,"Set"};
    const MenuEntry *entries[1];
    Menu *m=(Menu *)calloc(1,sizeof(*m));
    MenuPosition restored;
    int mode;
    assert(m);
    for (mode=0;mode<6;++mode) {
        memset(m,0,sizeof(*m));
        entered=loaded=mounted=applied=stopped=writes=0;
        saved[0]=recent[0]=0;fail_apply=mode==2;fail_mount=mode==3;
        m->ready=m->view_valid=m->playlist.synchronized=1;
        m->ode.transfer=transfer;m->depth=2;m->selected=1;
        m->path_ids[0]=10;m->path_ids[1]=11;
        strcpy(m->path[0],"Games");strcpy(m->path[1],"USA");
        m->metadata.entries=games;m->metadata.count=3;
        entries[0]=mode==1 ? &set : &one;
        m->listing.entries=entries;m->listing.count=1;m->listing.has_parent=1;
        m->config.remember_position=1;m->position_dirty=1;
        if (mode==4) m->single_disc=1; /* Explicit C+A still opens the folder. */
        if (mode==5) assert(!ode_playlist_append_local(&m->ode,&m->playlist,"/Storage/Existing.iso"));
        activate(m);
        if (mode==4) {
            assert(entered==1 && loaded==1 && m->depth==3 && !mounted && !writes);
        } else {
            assert(!entered && !loaded && m->depth==2 && m->selected==1);
            assert(!strcmp(m->path[1],"USA"));
            if (mode==2 || mode==5) assert(!mounted && !writes && !m->launching);
            else {
                assert(mounted==1 && writes==1);
                assert(!menu_position_parse(&restored,saved,strlen(saved)));
                assert(!strcmp(restored.folder,"/Storage/Games/USA"));
                assert(!strcmp(restored.selected,mode==1 ? "Set" : "One"));
                assert(!strcmp(recent,mode==1 ? "/Storage/Games/USA/Set/Set (Disc 1).iso" : "/Storage/Games/USA/One/One.iso"));
                assert(m->playlist.count==(mode==1 ? 2U : 1U));
                if (mode==1) assert(!strcmp(m->playlist.paths[1],"/Storage/Games/USA/Set/Set (Disc 2).iso"));
                assert(m->launching==(mode!=3));
                if (mode!=3) { remember_position(m,0);assert(writes==1); }
            }
        }
        /* The original browser listing is borrowed by this harness. */
        m->listing.entries=NULL;m->listing.count=0;
        ode_playlist_dispose(&m->playlist);
    }
    free(m);
    puts("Game-folder launch: single/multi-disc games keep browser and reboot position; failures, existing queue and explicit open passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='oh3do-game-folder-') as directory:
    code, binary = Path(directory) / 'test.c', Path(directory) / 'test'
    # The launch controller frees its listing after a successful mount. Borrowed
    # entries remain owned by this test, so only stub this allocation operation.
    code.write_text(stubs + '\n#define ode_listing_free(p) ((void)(p))\n' + controller + checks)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c89', '-Wall', '-Wextra', '-Werror', '-Isrc', str(code),
                    'src/ode.c', 'src/library_features.c', 'src/menu_ui.c', 'src/menu_categories.c', 'src/menu_position.c', 'src/menu_sort.c', 'src/cover_catalog.c',
                    '-o', str(binary)], cwd=root, check=True)
    subprocess.run([str(binary)], check=True)
