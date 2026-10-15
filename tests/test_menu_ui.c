#include "menu_ui.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

const MenuCover menu_covers[] = {
    {"Games/Disc 1.iso", "$boot/covers/one.cel"},
    {"Other/Disc 1.iso", "$boot/covers/two.cel"},
    {"Road Rash.iso", "$boot/covers/road.cel"}
};
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
    assert(menu_support_entry("Covers", 1, 1));
    assert(menu_support_entry("saves", 1, 1));
    assert(menu_support_entry("ohmy3do.ini", 0, 1));
    assert(!menu_support_entry("covers", 1, 0));
    assert(!menu_support_entry("boot.iso", 0, 0));
    assert(!menu_support_entry("saves.iso", 0, 1));
    assert(!menu_support_entry("covers", 0, 1));
    puts("menu_ui: display titles, scroll timing, breadcrumbs and exact cover paths passed");
    return 0;
}
