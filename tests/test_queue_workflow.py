#!/usr/bin/env python3
"""Run the actual menu queue-rebuild controller with failures at each visit."""
from pathlib import Path
import os
import subprocess
import tempfile
root=Path(__file__).resolve().parent.parent
source=(root/'src/main.c').read_text()
controller=source[source.index('static void current_route('):source.index('static int offer_group(Menu *menu, const MenuEntry *entry)\n{')]
stubs=r'''
#include "library_navigation.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define MAX_DEPTH 32
typedef struct Menu {
    Ode ode; OdeListing listing; OdePlaylist playlist,group;
    unsigned int depth;
    unsigned long path_ids[MAX_DEPTH]; char path[MAX_DEPTH][ODE_MAX_NAME];
    struct { int timed_out; } bus;
    struct { int folder_workaround; } config;
    const char *message;
} Menu;
static int visits,fail_visit,clears,adds,preflights;
static char remote[16][128];
static void draw(Menu *m) { (void)m; }
static int transfer(void *context,const unsigned char *p,unsigned char *r,size_t cap,size_t *size) {
    (void)context; memset(r,0,cap); r[0]=p[0];
    if(p[0] == 0xc3) { assert(preflights == 2); ++clears; adds=0; *size=2; }
    else if(p[0] == 0xc1) *size=18;
    else if(p[0] == 0xc4) { ++adds; r[1]=r[2]=1; *size=4; }
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
'''
checks=r'''
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
    puts("Queue workflow: all paths checked before clear; edited order applied; failed rebuilds cannot launch");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='oh3do-queue-') as directory:
    path,binary=Path(directory)/'test.c',Path(directory)/'test'
    path.write_text(stubs+controller+checks)
    subprocess.run([os.environ.get('CC','cc'),'-std=c89','-Wall','-Wextra','-Werror','-Isrc',str(path),'src/ode.c','src/menu_sort.c','-o',str(binary)],cwd=root,check=True)
    subprocess.run([str(binary)],check=True)
