#!/usr/bin/env python3
"""Exercise the menu's actual favorites edit callback with USB failures."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
source = (root / "src/main.c").read_text()
read = source[source.index("static int favorites_read("):source.index("static void open_favorites(Menu *menu)\n{")]
edit = source[source.index("typedef struct FavoritesEdit"):source.index("static void edit_favorites(")]
harness = r'''
#include "library_features.h"
#include "usb_file.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef struct Menu { Ode ode; MenuFavorites favorites; int favorites_valid; } Menu;
static char file[FAVORITES_BYTES];
static int reads, writes, read_fail, write_fail;
static int ode_bus_read_buffer(void *context, unsigned char *data, size_t size)
{ (void)context; (void)data; (void)size; return -1; }
int usb_file_text(Ode *ode, OdeReadBuffer reader, const char *name, char *out, size_t cap, size_t *length)
{
    (void)ode; (void)reader; assert(!strcmp(name,"favorites.txt")); ++reads;
    if (read_fail) return -1;
    *length=strlen(file); assert(*length < cap); strcpy(out,file); return 0;
}
int usb_file_text_write(Ode *ode, OdeReadBuffer reader, const char *name, const char *text)
{
    (void)ode; (void)reader; assert(!strcmp(name,"favorites.txt")); ++writes;
    if (write_fail) return -1;
    strcpy(file,text); return 0;
}
'''
checks = r'''
int main(void)
{
    Menu menu; FavoritesEdit op; int before;
    memset(&menu,0,sizeof(menu)); op.menu=&menu; op.path="New.iso"; op.remove=0;
    assert(!favorites_edit_at_root(&op) && !strcmp(file,"New.iso\n") && reads == 1 && writes == 1);
    before=writes; assert(favorites_edit_at_root(&op) == 1 && writes == before);
    /* Changes made on a PC since opening the screen must survive the next edit. */
    strcpy(file,"External.iso\nNew.iso\n"); op.path="Third.iso";
    assert(!favorites_edit_at_root(&op) && !strcmp(file,"External.iso\nNew.iso\nThird.iso\n"));
    op.path="new.ISO"; op.remove=1;
    assert(!favorites_edit_at_root(&op) && !strcmp(file,"External.iso\nThird.iso\n"));
    strcpy(file,"../invalid.iso\n"); before=writes;
    assert(favorites_edit_at_root(&op) < 0 && writes == before && !menu.favorites_valid);
    read_fail=1; assert(favorites_edit_at_root(&op) < 0 && writes == before);
    read_fail=0; file[0]=0; write_fail=1; op.remove=0;
    assert(favorites_edit_at_root(&op) < 0 && !menu.favorites_valid);
    /* A hidden favorite remains in the complete list through removal/addition. */
    strcpy(file,"Games/A.iso\nAdult/Japan/Hidden.iso\nGames/B.iso\n");
    write_fail=0; op.path="Games/A.iso"; op.remove=1;
    assert(!favorites_edit_at_root(&op));
    assert(!strcmp(file,"Adult/Japan/Hidden.iso\nGames/B.iso\n"));
    assert(menu_favorites_visible_count(&menu.favorites,1)==1);
    assert(menu_favorites_visible_index(&menu.favorites,1,0)==1);
    assert(menu_favorites_visible_index(&menu.favorites,1,1)==menu.favorites.count);
    assert(menu_favorites_visible_count(&menu.favorites,0)==2);
    op.path="Games/C.iso";op.remove=0;
    assert(!favorites_edit_at_root(&op) && strstr(file,"Adult/Japan/Hidden.iso\n"));
    assert(menu_favorites_visible_count(&menu.favorites,1)==2);
    menu_favorites_free(&menu.favorites);
    puts("Favorites workflow: create, fresh-read edits, duplicates, removal, invalid files and USB failures passed");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="oh3do-favorites-") as directory:
    code, binary = Path(directory) / "test.c", Path(directory) / "test"
    code.write_text(harness + read + edit + checks)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c89", "-Wall", "-Wextra", "-Werror",
                    "-I", str(root / "src"), str(code), str(root / "src/library_features.c"),
                    str(root / "src/menu_ui.c"), str(root / "src/cover_catalog.c"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
