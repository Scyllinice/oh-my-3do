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
    menu_favorites_free(&f);
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
