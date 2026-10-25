#include "menu_ui.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

const MenuCover menu_covers[] = {{"Games/Disc 1.iso", "$boot/covers/one.cel"},
                                 {"Other/Disc 1.iso", "$boot/covers/two.cel"},
                                 {"Road Rash.iso", "$boot/covers/road.cel"}};
const size_t menu_cover_count = 3;

int main(void)
{
    char breadcrumb[35];
    char long_name[1024];
    assert(menu_title_length("Disc 1.ISO", 0) == 6);
    assert(menu_title_length("Disc 1.cUe", 0) == 6);
    assert(menu_title_length("Folder.iso", 1) == 10);
    assert(menu_title_length("Game.iso.bak", 0) == 12);
    assert(menu_title_length(".iso", 0) == 4);
    assert(menu_title_length("Games/Disc 1.iso", 0) == 12);
    assert(menu_short_title_length("Road Rash (USA).iso", 0) == 9);
    assert(menu_short_title_length("MegaRace (USA, Europe) (En,Fr,De,Es,It).iso", 0) == 8);
    assert(menu_short_title_length("Return Fire - Maps O' Death (USA)", 0) == 27);
    assert(menu_short_title_length("Gex.iso", 0) == 3);
    assert(menu_short_title_length("(USA).iso", 0) == 5);     /* nothing would remain */
    assert(menu_short_title_length("Game(USA).iso", 0) == 9); /* no space: not a tag */
    assert(menu_short_title_length("Odd) (USA)", 0) == 4);
    assert(menu_short_title_length("Japan (Imports)", 1) == 15);
    {
        static const char *const cases[][2] = {
            {"Daedalus Encounter, The (USA, Europe).iso", "Daedalus Encounter, The"},
            {"Game (USA) (Not for Resale).iso", "Game (Not for Resale)"},
            {"Game (Japan) (RE-1) (En,Ja).cue", "Game (RE-1)"},
            {"Game (Europe) (RE) (RE1) (RE2) (Unl) (Alt) (Rev A)",
             "Game (RE) (RE1) (RE2) (Unl) (Alt) (Rev A)"},
            {"Game (USA) (Disc 1) (Demo) (Proto) (Beta) (Sample)",
             "Game (Disc 1) (Demo) (Proto) (Beta) (Sample)"},
            {"Game [Japan] [Unl]", "Game [Unl]"},
            {"Game (En,Ja,Fr,De,Es,It,Pt,Ko,Ru,Ar)", "Game"},
            {"Game (Unknown Edition) (USA)", "Game (Unknown Edition)"},
            {"Game (USA, Rev 1)", "Game (USA, Rev 1)"},
            {"Game (The Game)", "Game (The Game)"},
            {"Game (USA) Bonus", "Game (USA) Bonus"},
            {"(USA).iso", "(USA)"},
            {"Game (USA,)", "Game (USA,)"},
            {"Game (USA", "Game (USA"}};
        char out[256], tiny[5];
        size_t i;
        for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            assert(menu_display_title(out, sizeof(out), cases[i][0], 0) == strlen(cases[i][1]));
            assert(!strcmp(out, cases[i][1]));
        }
        menu_display_title(out, sizeof(out), "Category (Japan)", 1);
        assert(!strcmp(out, "Category (Japan)"));
        memset(tiny, 'x', sizeof(tiny));
        menu_display_title(tiny, 4, "Game (USA) (Unl)", 0);
        assert(!strcmp(tiny, "Gam") && tiny[4] == 'x');
        assert(!menu_display_title(tiny, 0, "Game", 0) && tiny[4] == 'x');
    }
    assert(!strcmp(menu_image_format("Games/Disc 1.ISO", 0), "ISO"));
    assert(!strcmp(menu_image_format("Games/Disc 1.cUe", 0), "CUE"));
    assert(!menu_image_format("Folder.cue", 1));
    assert(!menu_image_format("Game.iso.bak", 0));
    assert(!menu_image_format(".cue", 0));
    assert(menu_scroll_offset(32, 32, 100000UL) == 0);
    assert(menu_scroll_offset(34, 32, 0) == 0);
    assert(menu_scroll_offset(34, 32, 1199) == 0);
    assert(menu_scroll_offset(34, 32, 1380) == 1);
    assert(menu_scroll_offset(34, 32, 1560) == 2);
    assert(menu_scroll_offset(34, 32, 2559) == 2);
    assert(menu_scroll_offset(34, 32, 2560) == 0);
    menu_breadcrumb(breadcrumb, "", "", 0);
    assert(!strcmp(breadcrumb, "/ STORAGE"));
    menu_breadcrumb(breadcrumb, "", "Games", 1);
    assert(!strcmp(breadcrumb, "/ Games"));
    menu_breadcrumb(breadcrumb, "Games", "Favorites", 2);
    assert(!strcmp(breadcrumb, "... / Games / Favorites"));
    memset(long_name, 'x', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = 0;
    menu_breadcrumb(breadcrumb, "Games", long_name, 2);
    assert(strlen(breadcrumb) == 34 && !strcmp(breadcrumb + 31, "..."));
    menu_breadcrumb(breadcrumb, long_name, "Games", 3);
    assert(!strcmp(breadcrumb, "... / Games"));
    assert(!strcmp(menu_cover_lookup("/Storage/games/DISC 1.ISO"), "$boot/covers/one.cel"));
    assert(!strcmp(menu_cover_lookup("Other/Disc 1.iso"), "$boot/covers/two.cel"));
    assert(!strcmp(menu_cover_lookup("/Road Rash.iso"), "$boot/covers/road.cel"));
    assert(!menu_cover_lookup("Disc 1.iso"));
    assert(!menu_cover_lookup("Games/Disc 2.iso"));
    assert(menu_support_entry("BOOT.ISO", 0, 1));
    assert(menu_support_entry("menu-demo.iso.sha256", 0, 1));
    assert(!menu_support_entry("covers", 1, 1));
    assert(!menu_support_entry("ohmy3do.ini", 0, 1));
    assert(menu_support_entry(".OhMy3DO", 1, 1));
    assert(!menu_support_entry("Adult", 1, 1));
    menu_set_hide_adult(1);
    assert(menu_support_entry("ADULT", 1, 1) && !menu_support_entry("Adult", 1, 0) &&
           !menu_support_entry("Adult.iso", 0, 1));
    menu_set_hide_adult(0);
    assert(menu_adult_path("Adult/Japan/Game/Game.iso") && menu_adult_path("/Storage/adult/x.iso"));
    assert(menu_adult_path("/Storage/USB A/Adult/Japan/x.iso") &&
           !menu_adult_path("/Storage/USB A/Games/Adult/x.iso"));
    assert(!menu_adult_path("Games/Adult/x.iso") && !menu_adult_path("Adultery.iso") &&
           !menu_adult_path("/Storage/Adult"));
    assert(!menu_support_entry(".ohmy3do", 1, 0));
    assert(!menu_support_entry("boot.iso", 0, 0));
    assert(!menu_support_entry("saves.iso", 0, 1));
    assert(!menu_support_entry("covers", 0, 1));
    assert(menu_support_entry("Game (Track 1).BIN", 0, 0));
    assert(menu_support_entry("Game.bin", 0, 1));
    assert(!menu_support_entry("Game.bin", 1, 0));
    assert(!menu_support_entry("Game.bin.bak", 0, 0));
    assert(!menu_support_entry("Game.cue", 0, 0));
    assert(!menu_support_entry("Game.iso", 0, 0));
    assert(menu_support_entry("OH MY 3DO - FOLDER NAVIGATION HELPER (KEEP).ISO", 0, 0));
    assert(menu_support_entry("Oh My 3DO - Folder Navigation Helper (Keep).iso", 0, 1));
    assert(!menu_support_entry("Oh My 3DO - Folder Navigation Helper (Keep).iso", 1, 0));
    puts("menu_ui: display titles, scroll timing, breadcrumbs and exact cover paths passed");
    return 0;
}
