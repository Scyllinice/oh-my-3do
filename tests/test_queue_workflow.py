#!/usr/bin/env python3
"""Run the actual menu queue-rebuild controller with failures at each visit."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
source = (root / "src/main.c").read_text()
controller = source[
    source.index("static void current_route(Menu *menu, ArtRouteStep *route)\n{") : source.index(
        "static int queue_set(Menu *menu, const MenuEntry *entry)\n{"
    )
]
stubs = r"""
#include "library_navigation.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_DEPTH 32
#define FOLDER_PATH_BYTES 8193
#define ENTRY_PATH_BYTES 9218
#define STORAGE_PREFIX 9
typedef struct Menu {
    Ode ode; OdeListing listing; OdePlaylist playlist,group;
    unsigned int depth;
    unsigned long path_ids[MAX_DEPTH]; char path[MAX_DEPTH][ODE_MAX_NAME];
    struct { int timed_out; } bus;
    struct { int folder_workaround; } config;
    const char *message;
} Menu;
static int folder_workaround(const Menu *m) { return m->config.folder_workaround; }
static int visits,fail_visit,clears,adds,preflights,local_mode,fresh_reads;
static int directory_path(Menu *m,char *out,size_t capacity) { (void)m;(void)capacity; strcpy(out,"/Storage/Games"); return local_mode ? 0 : -1; }
int menu_path_compare(const char *a,const char *b) { return strcmp(a,b); }
static int queue_read_directory(Ode *ode,OdeListing *listing) {
    size_t i; static const char *names[]={"Disc 1.iso","Disc 2.iso","Disc 3.iso","Disc 4.iso"};
    (void)ode; ++fresh_reads;
    listing->entries=(const MenuEntry **)calloc(4,sizeof(MenuEntry *)); assert(listing->entries);
    for (i=0;i<4;++i) {
        MenuEntry *e=(MenuEntry *)calloc(1,sizeof(*e)); assert(e);
        e->toc_id=i+1; e->name=names[i]; listing->entries[listing->count++]=e;
    }
    return 0;
}
#define ode_read_directory queue_read_directory
static char remote[16][128];
static void draw(Menu *m) { (void)m; }
static int transfer(void *context,const unsigned char *p,unsigned char *r,size_t cap,size_t *size) {
    (void)context; memset(r,0,cap); r[0]=p[0];
    if(p[0] == 0xc3) { assert(local_mode ? fresh_reads==1 : preflights==2); ++clears; adds=0; *size=2; }
    else if(p[0] == 0xc1) *size=18;
    else if(p[0] == 0xc4) { if(local_mode) sprintf(remote[adds],"/Storage/Games/Disc %u.iso",p[4]); ++adds; r[1]=r[2]=1; *size=4; }
    else assert(0);
    return 0;
}
int library_visit(Ode *ode,const ArtRouteStep *route,unsigned int depth,const OdeListing *current,
                  int workaround,const char *path,LibraryFileAction action,void *context) {
    MenuEntry e; int result;
    (void)route;(void)depth;(void)current;(void)workaround;
    if(++visits == fail_visit) { ode->error="missing path"; return -1; }
    if(!action) { assert(!clears); ++preflights; return 0; }
    assert(clears == 1 && preflights == 2);
    e.toc_id=2; e.is_directory=0; e.name=strrchr(path,'/')+1;
    strcpy(remote[adds],path);
    result=action(context,&e,"/Storage/Games");
    return result;
}
"""
checks = r"""
int main(void) {
    Menu m; int fail;
    for(fail=0;fail<=4;++fail) {
        memset(&m,0,sizeof(m)); visits=clears=adds=preflights=0; fail_visit=fail;
        m.ode.transfer=transfer; m.playlist.synchronized=1;
        assert(!ode_playlist_append_local(&m.ode,&m.playlist,"/Storage/Games/Disc 1.iso"));
        assert(!ode_playlist_append_local(&m.ode,&m.playlist,"/Storage/Games/Disc 2.iso"));
        assert(!ode_playlist_move_local(&m.playlist,1,0));
        if(!fail) {
            assert(!apply_queue(&m) && !m.playlist.dirty && m.playlist.synchronized && adds == 2);
            assert(!strcmp(remote[0],m.playlist.paths[0]) && !strcmp(remote[1],m.playlist.paths[1]));
        } else {
            assert(apply_queue(&m) < 0 && m.playlist.dirty && m.playlist.count == 2);
            if(fail <= 2) assert(!clears && m.playlist.synchronized);
            else assert(clears == 1 && !m.playlist.synchronized);
        }
        assert(!strcmp(m.playlist.paths[0],"/Storage/Games/Disc 2.iso"));
        ode_playlist_dispose(&m.playlist); ode_playlist_dispose(&m.group);
    }
    memset(&m,0,sizeof(m)); visits=clears=adds=preflights=fresh_reads=0; local_mode=1;
    m.ode.transfer=transfer; m.playlist.synchronized=1;
    queue_read_directory(&m.ode,&m.listing); fresh_reads=0;
    assert(!ode_playlist_append_local(&m.ode,&m.playlist,"/Storage/Games/Disc 1.iso"));
    assert(!ode_playlist_append_local(&m.ode,&m.playlist,"/Storage/Games/Disc 2.iso"));
    assert(!ode_playlist_append_local(&m.ode,&m.playlist,"/Storage/Games/Disc 3.iso"));
    assert(!ode_playlist_append_local(&m.ode,&m.playlist,"/Storage/Games/Disc 4.iso"));
    assert(!ode_playlist_move_local(&m.playlist,3,0));
    assert(!apply_queue(&m) && !visits && fresh_reads==1 && adds==4 && clears==1);
    assert(!strcmp(remote[0],m.playlist.paths[0]));
    assert(!strcmp(remote[3],m.playlist.paths[3]));
    clears=adds=fresh_reads=0;
    assert(!ode_playlist_append_local(&m.ode,&m.playlist,"/Storage/Games/Missing.iso"));
    assert(apply_queue(&m)<0 && !clears && !adds && !fresh_reads && !visits && m.playlist.synchronized);
    ode_playlist_dispose(&m.playlist); ode_playlist_dispose(&m.group); ode_listing_free(&m.listing);
    puts("Same-folder four-disc queue: one fresh listing, no folder walks, reordered sequence preserved");
    puts("Queue workflow: all paths checked before clear; edited order applied; failed rebuilds cannot launch");
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="oh3do-queue-") as directory:
    path, binary = Path(directory) / "test.c", Path(directory) / "test"
    navigation = (root / "src/library_navigation.c").read_text()
    resolver = navigation[navigation.index("int library_current_files(") :]
    path.write_text(stubs + resolver + controller + checks)
    subprocess.run(
        [
            os.environ.get("CC", "cc"),
            "-std=c89",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Isrc",
            str(path),
            "src/ode.c",
            "src/menu_sort.c",
            "-o",
            str(binary),
        ],
        cwd=root,
        check=True,
    )
    subprocess.run([str(binary)], check=True)
