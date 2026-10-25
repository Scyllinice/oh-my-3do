#!/usr/bin/env python3
"""Exercise the menu's actual lookup/cache functions and owner invalidation."""
from pathlib import Path
import re
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
source = (root / 'src/main.c').read_text()
def find(marker, after=0):
    """Where `marker` appears in main.c, ignoring differences in whitespace."""
    pattern = r'\s*'.join(re.escape(c) for c in marker if not c.isspace())
    match = re.compile(pattern).search(source, after)
    if not match:
        raise ValueError(f'Not found in main.c: {marker!r}')
    return match.start()
def section(start, end):
    begin = find(start)
    return source[begin:find(end, begin)]
types = section('#define GAME_LOOKUP_SLOTS', '#define SCROLL_MEASURE_SLOTS')
fields = section('    GameLookup game_lookups[GAME_LOOKUP_SLOTS];', '    ScrollMeasure scroll_measures')
controller = section('static long relative_folder(Menu *menu,char *path,size_t size,size_t room)\n{', 'static int compare_title_hash')
stubs = r'''
#include "library_index.h"
#include "library_features.h"
#include "menu_ui.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
/*TYPES*/
typedef struct Menu {
    LibraryMetadata metadata;
    unsigned long listing_generation;
    unsigned int depth;
    unsigned long path_ids[32];
    char path[32][ODE_MAX_NAME];
/*FIELDS*/
} Menu;
static unsigned int groups,finds;
size_t __real_menu_disc_group(const OdeListing *,const MenuEntry *,const MenuEntry **,size_t,size_t *);
size_t __wrap_menu_disc_group(const OdeListing *l,const MenuEntry *e,const MenuEntry **out,size_t cap,size_t *at) {
    ++groups;return __real_menu_disc_group(l,e,out,cap,at);
}
const LibraryGameInfo *__real_library_metadata_find(const LibraryMetadata *,const char *);
const LibraryGameInfo *__wrap_library_metadata_find(const LibraryMetadata *m,const char *path) {
    ++finds;return __real_library_metadata_find(m,path);
}
'''.replace('/*TYPES*/', types).replace('/*FIELDS*/', fields)
checks = r'''
int main(void) {
    static Menu m;
    LibraryGameInfo games[]={
        {"Games/USA/Direct.iso","Action","Publisher","1995"},
        {"Games/USA/One/One.iso","Adventure","Publisher","1995"},
        {"Games/USA/Ordinary/Nested/Nested.iso","","",""},
        {"Games/USA/Set/Set (Disc 1).iso","Action","Publisher","1995"},
        {"Games/USA/Set/Set (Disc 2).iso","Action","Publisher","1995"}
    }, replacement[5];
    const MenuEntry one={1,1,"One"},set={2,1,"Set"},ordinary={3,1,"Ordinary"},direct={4,0,"Direct.iso"};
    MenuEntry extra[GAME_LOOKUP_SLOTS];
    char names[GAME_LOOKUP_SLOTS][24];
    const char *discs[ODE_MAX_DISCS];
    const LibraryGameInfo *info;
    unsigned int i;
    m.depth=2;strcpy(m.path[0],"Games");strcpy(m.path[1],"USA");m.path_ids[0]=1;
    m.metadata.entries=games;m.metadata.count=5;
    assert(!folder_game(&m,NULL,discs) && !game_info(&m,NULL) && !game_entry(&m,NULL));
    assert(folder_game(&m,&set,discs)==2 && groups==1);
    assert(!strcmp(discs[0],"Set (Disc 1).iso") && !strcmp(discs[1],"Set (Disc 2).iso"));
    for (i=0;i<100;++i) {
        assert(game_entry(&m,&set));assert(folder_game(&m,&set,discs)==2);
        assert(game_info(&m,&set)==games+3);
    }
    assert(groups==1 && finds==1); /* repeated rendering did not resolve again */
    assert(game_info(&m,&one)==games+1);assert(game_info(&m,&set)==games+3);
    assert(finds==2 && groups==1); /* backtracking reuses the same cached result */
    assert(game_entry(&m,&direct));assert(game_info(&m,&direct)==games);
    assert(!game_entry(&m,&ordinary) && !game_info(&m,&ordinary));
    i=finds;assert(!game_info(&m,&ordinary) && finds==i);
    assert(game_info(&m,&set)==games+3);
    ++m.listing_generation;assert(game_info(&m,&set)==games+3 && groups==2);
    /* Same allocation and count after reload: explicit invalidation is required. */
    games[3].path="Games/USA/Set/New Set (Disc 1).iso";
    games[4].path="Games/USA/Set/New Set (Disc 2).iso";m.game_lookups_valid=0;
    assert(folder_game(&m,&set,discs)==2 && groups==3);
    assert(!strcmp(discs[1],"New Set (Disc 2).iso"));
    memcpy(replacement,games,sizeof(games));m.metadata.entries=replacement;
    assert(game_info(&m,&set)==replacement+3 && groups==4);
    m.metadata.count=4;assert(folder_game(&m,&set,discs)==1);
    m.metadata.count=5;assert(folder_game(&m,&set,discs)==2 && groups==5);
    m.depth=1;assert(!folder_game(&m,&set,discs));
    m.depth=2;assert(folder_game(&m,&set,discs)==2 && groups==6);
    /* A reused entry pointer in another directory must not retain old paths. */
    strcpy(m.path[1],"Europe");++m.listing_generation;
    assert(!folder_game(&m,&set,discs) && !game_info(&m,&set));
    strcpy(m.path[1],"USA");++m.listing_generation;
    assert(game_info(&m,&set)==replacement+3 && groups==7);
    for (i=0;i<GAME_LOOKUP_SLOTS;++i) {
        sprintf(names[i],"Folder%u",i);extra[i].name=names[i];extra[i].is_directory=1;extra[i].toc_id=i+10;
        assert(!game_entry(&m,extra+i));
    }
    info=game_info(&m,&set);assert(info==replacement+3 && groups==8); /* evicted, then resolved safely */
    puts("Game lookup cache: repeated hits, disc order, missing metadata, eviction and listing/index/depth invalidation passed");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='oh3do-lookups-') as directory:
    code, binary = Path(directory) / 'test.c', Path(directory) / 'test'
    code.write_text(stubs + controller + checks)
    subprocess.run([os.environ.get('CC', 'cc'), '-O2', '-std=c89', '-pedantic', '-Wall', '-Wextra', '-Werror',
                    '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-Wl,--wrap=menu_disc_group', '-Wl,--wrap=library_metadata_find',
                    '-Isrc', str(code), 'src/library_index.c', 'src/library_features.c', 'src/menu_sort.c',
                    'src/menu_ui.c', 'src/menu_categories.c', 'src/cover_catalog.c', '-o', str(binary)], cwd=root, check=True)
    subprocess.run([str(binary)], check=True)
