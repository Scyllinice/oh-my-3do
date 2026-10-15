#include "library_features.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    MenuFavorites f;
    OdeListing listing;
    static const MenuEntry d1={1,0,"D (Disc 1).iso"}, d2={2,0,"d (Disc 2).iso"}, d3={3,0,"D (Disc 3).iso"};
    static const MenuEntry wrong={4,0,"Other (Disc 2).iso"}, duplicate={5,0,"D (Disc 02).cue"};
    const MenuEntry *entries[]={&d3,&wrong,&d1,&d2}, *out[16];
    OdePlaylist q; Ode ode;
    memset(&f,0,sizeof(f)); memset(&ode,0,sizeof(ode)); memset(&listing,0,sizeof(listing));
    assert(!menu_favorites_parse(&f,"# Favorites\r\nGames/D (Disc 1).iso\nRoad Rash.iso\n",strlen("# Favorites\r\nGames/D (Disc 1).iso\nRoad Rash.iso\n")));
    assert(f.count == 2 && !strcmp(f.paths[0],"Games/D (Disc 1).iso"));
    assert(menu_favorites_parse(&f,"../bad.iso",10) < 0 && f.count == 2);
    assert(menu_favorites_parse(&f,"Game.iso\ngame.ISO",17) < 0);
    assert(!menu_relative_path("/absolute.iso") && !menu_relative_path("a//b") && !menu_relative_path("a\\b"));
    assert(!menu_favorites_edit(&f,"Games/New.iso",0) && f.count == 3);
    assert(menu_favorites_edit(&f,"games/NEW.ISO",0) == 1 && f.count == 3);
    assert(!menu_favorites_edit(&f,"games/d (disc 1).ISO",1) && f.count == 2);
    assert(!strcmp(f.paths[0],"Road Rash.iso") && !strcmp(f.paths[1],"Games/New.iso"));
    assert(menu_favorites_edit(&f,"Missing.iso",1) == 1 && f.count == 2);
    assert(menu_favorites_edit(&f,"#Bad.iso",0) < 0 && f.count == 2);
    assert(menu_favorites_edit(&f," leading.iso",0) < 0 && f.count == 2);
    assert(menu_favorites_edit(&f,"trailing.iso ",0) < 0 && f.count == 2);
    assert(!menu_favorites_edit(&f,"Road Rash.iso",1));
    assert(!menu_favorites_edit(&f,"Games/New.iso",1) && !f.count);
    assert(!menu_favorites_edit(&f,"First.iso",0) && f.count == 1);
    menu_favorites_free(&f);
    {
        char path[FAVORITE_PATH]; size_t j;
        for (j=0;j<FAVORITES_MAX;++j) {
            sprintf(path,"Game %lu.iso",(unsigned long)j);
            assert(!menu_favorites_edit(&f,path,0));
        }
        assert(menu_favorites_edit(&f,"Overflow.iso",0) < 0 && f.count == FAVORITES_MAX);
        menu_favorites_free(&f);
        memset(path,'a',1000); path[1000]=0;
        for (j=0;j<8;++j) { path[0]=(char)('A'+j); assert(!menu_favorites_edit(&f,path,0)); }
        path[0]='Z'; assert(menu_favorites_edit(&f,path,0) < 0 && f.count == 8);
        menu_favorites_free(&f);
    }
    {
        char text[FAVORITES_BYTES], path[32]; size_t j;
        assert(menu_name_matches("Dragon Disc 10.ISO","disc 1"));
        assert(!menu_name_matches("D (Disc 2).iso","disc 1") && menu_name_matches("anything",""));
        assert(!menu_favorites_parse(&f,"First.iso\nSecond.iso\nThird.iso\n",strlen("First.iso\nSecond.iso\nThird.iso\n")));
        assert(!menu_favorites_move(&f,0,2) && !strcmp(f.paths[2],"First.iso"));
        assert(!menu_favorites_move(&f,2,0) && !strcmp(f.paths[0],"First.iso"));
        assert(menu_favorites_move(&f,0,3) < 0);
        assert(!menu_favorites_format(&f,text,sizeof(text)) && !strcmp(text,"First.iso\nSecond.iso\nThird.iso\n"));
        assert(menu_favorites_format(&f,text,3) < 0);
        menu_favorites_free(&f);
        for (j=0;j<15;++j) { sprintf(path,"Game %lu.iso",(unsigned long)j); assert(!menu_recent_push(&f,path)); }
        assert(f.count == 10 && !strcmp(f.paths[0],"Game 14.iso") && !strcmp(f.paths[9],"Game 5.iso"));
        assert(!menu_recent_push(&f,"GAME 10.ISO") && f.count == 10 && !strcmp(f.paths[0],"GAME 10.ISO"));
        assert(menu_recent_push(&f,"../bad.iso") < 0 && f.count == 10);
        menu_favorites_free(&f);
    }
    listing.entries=entries; listing.count=4;
    assert(menu_disc_group(&listing,&d1,out,16) == 3 && out[0] == &d1 && out[1] == &d2 && out[2] == &d3);
    assert(!menu_disc_group(&listing,&d2,out,16));
    entries[3]=&duplicate; entries[1]=&d2;
    assert(!menu_disc_group(&listing,&d1,out,16));
    listing.count=3; entries[0]=&d3; entries[1]=&wrong; entries[2]=&d1;
    assert(!menu_disc_group(&listing,&d1,out,16));
    assert(menu_initial("d.iso") == 4 && menu_initial("2 Games") == 0);
    ode_playlist_init(&q); q.synchronized=1;
    assert(!ode_playlist_append_local(&ode,&q,"/Storage/D1.iso"));
    assert(!ode_playlist_append_local(&ode,&q,"/Storage/D2.iso"));
    assert(!ode_playlist_append_local(&ode,&q,"/Storage/D3.iso"));
    assert(ode_playlist_append_local(&ode,&q,"/Storage/D2.iso") < 0);
    assert(!ode_playlist_move_local(&q,2,0)); assert(!strcmp(q.paths[0],"/Storage/D3.iso"));
    assert(!ode_playlist_move_local(&q,0,2)); assert(!strcmp(q.paths[0],"/Storage/D1.iso"));
    assert(!ode_playlist_remove_local(&q,1) && q.count == 2 && q.dirty && !strcmp(q.paths[1],"/Storage/D3.iso"));
    assert(ode_playlist_start(&ode,&q) < 0); /* Never launch an unapplied edit. */
    q.synchronized=0; assert(ode_playlist_remove_local(&q,0) < 0 && ode_playlist_move_local(&q,0,1) < 0);
    ode_playlist_dispose(&q);
    puts("Library features: favorites validation, conservative disc grouping and local queue edits passed");
    return 0;
}
