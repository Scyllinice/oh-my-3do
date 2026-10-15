#!/usr/bin/env python3
"""Exercise the actual startup controller and its display/navigation ordering."""
from pathlib import Path
import os
import subprocess
import tempfile
root=Path(__file__).resolve().parent.parent
source=(root/'src/main.c').read_text()
controller=source[source.index('static void connect_ode(Menu *menu)\n{'):source.index('static void go_back(Menu *menu)\n{')]
stubs=r'''
#include "library_navigation.h"
#include "library_features.h"
#include "menu_config.h"
#include "menu_position.h"
#include "usb_art.h"
#include "cover_cache.h"
#include "menu_music.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define ControlX 1
#define NULL_SPLASH ((void *)1)
typedef struct { unsigned int cped_ButtonBits; } ControlPadEventData;
typedef struct Menu {
    Ode ode; OdeListing listing; OdePlaylist playlist; MenuMusic music;
    MenuConfig config,saved_config; MenuPosition last_position; MenuFavorites recent;
    CoverCache cover_cache; LibraryCatalog catalog; UsbArtIndex usb_art,usb_previews;
    struct { int request,timed_out; } bus;
    int booting,review,demo,ready,view_valid,selected,depth,notice_active;
    int usb_music_result,usb_tested,backup_result,restore_result,position_result;
    int recent_result,diagnostics_result,recent_valid,config_result,skip_usb_art,covers_enabled;
    int settings_folder_workaround,settings_usb_covers,settings_remember_position;
    int settings_recent_games,settings_video_previews;
    void *splash; const char *message;
} Menu;
static int fail_stage,recovery,configured,positioned,shown,music_calls,splash_draws;
void menu_music_close(MenuMusic *m) { (void)m; }
void ode_playlist_dispose(OdePlaylist *p) { memset(p,0,sizeof(*p)); }
void library_catalog_free(LibraryCatalog *p) { memset(p,0,sizeof(*p)); }
void menu_favorites_free(MenuFavorites *p) { memset(p,0,sizeof(*p)); }
void usb_art_index_free(UsbArtIndex *p) { memset(p,0,sizeof(*p)); }
void ode_listing_free(OdeListing *p) { memset(p,0,sizeof(*p)); }
void cover_cache_clear(CoverCache *p) { (void)p; }
static void selection_changed(Menu *m) { (void)m; }
static void update_breadcrumb(Menu *m) { (void)m; }
static void draw(Menu *m) {
    if (m->booting) { ++splash_draws; assert(!shown); }
    else {
        ++shown; assert(!m->splash);
        if (m->ready && !recovery) assert(configured && positioned && m->config.screen_margin==12 && m->depth==3);
    }
}
void menu_config_defaults(MenuConfig *p) { memset(p,0,sizeof(*p)); p->remember_position=1; }
static int ode_bus_open(void *p) { (void)p; return fail_stage==1 ? -1 : 0; }
int ode_identify(Ode *p) { strcpy(p->identity,"LOCODE"); return fail_stage==2 ? -1 : 0; }
int ode_change_directory(Ode *p,unsigned long id) { (void)p; assert(!id); return 0; }
static int GetControlPad(int a,int b,ControlPadEventData *p) { (void)a;(void)b;p->cped_ButtonBits=recovery ? ControlX : 0;return 0; }
static int ode_bus_read_buffer(void *p,unsigned char *data,size_t n) { (void)p;(void)data;(void)n;return 0; }
int menu_config_load(Ode *ode,OdeReadBuffer reader,MenuConfig *p) {
    (void)ode;(void)reader; assert(!shown && !music_calls); ++configured;
    p->screen_margin=12;p->folder_workaround=1;p->remember_position=1;
    return 0;
}
int ode_playlist_clear(Ode *p,OdePlaylist *list) { (void)p;(void)list;return fail_stage==3 ? -1 : 0; }
static void load_directory(Menu *m,size_t selected) { assert(!selected && m->booting && !music_calls); m->ready=m->view_valid=fail_stage!=4; }
static void load_position(Menu *m) { assert(configured && m->booting && !shown && !music_calls); ++positioned;m->depth=3;m->selected=5; }
static void UnloadCel(void *p) { assert(p==NULL_SPLASH); }
static void sync_music(Menu *m) { assert(shown==1 && !m->booting && !m->splash); if(m->ready && !recovery) assert(positioned && m->selected==5); ++music_calls; }
'''
checks=r'''
int main(void) {
    Menu menu; int failure;
    for(failure=0;failure<=4;++failure) {
        memset(&menu,0,sizeof(menu));menu.bus.request=-1;menu.splash=NULL_SPLASH;
        fail_stage=failure;recovery=configured=positioned=shown=music_calls=splash_draws=0;
        connect_ode(&menu);
        assert(!menu.booting && !menu.splash && shown==1 && music_calls==1 && splash_draws);
        if(!failure) assert(positioned==1 && menu.saved_config.screen_margin==12);
    }
    memset(&menu,0,sizeof(menu)); menu.splash=NULL_SPLASH;
    fail_stage=0;recovery=1;configured=positioned=shown=music_calls=splash_draws=0;
    connect_ode(&menu);
    assert(!configured && !positioned && menu.config_result==2 && shown==1);
    puts("Startup: splash hides default layout; saved settings and folder precede menu/music; failures and X recovery passed");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='oh3do-startup-') as directory:
    code,binary=Path(directory)/'test.c',Path(directory)/'test'
    code.write_text(stubs+controller+checks)
    subprocess.run([os.environ.get('CC','cc'),'-std=c89','-Wall','-Wextra','-Werror','-Isrc',str(code),'-o',str(binary)],cwd=root,check=True)
    subprocess.run([str(binary)],check=True)
