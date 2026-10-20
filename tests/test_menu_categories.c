#include "menu_categories.h"
#include "menu_ui.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    const char *custom="{\"schema\":1,\"categories\":[{\"id\":\"custom\",\"name\":\"Favorites 2\",\"hidden\":true},{\"id\":\"games\",\"name\":\"Games\",\"hidden\":false}],\"defaults\":{\"Games\":\"games\"}}";
    char before[MENU_CATEGORIES_BYTES];
    menu_categories_reset();assert(menu_categories_count()==8);assert(!menu_category_hidden(0));
    assert(menu_category_toggle(0)==0);assert(menu_support_entry("Games",1,1));assert(menu_categories_hidden_path("Games/USA/Test/test.iso"));
    assert(menu_categories_hidden_path("/Storage/USB/Games/USA/Test/test.iso"));assert(menu_categories_hidden_path("USB/Games/USA/Test/test.iso"));assert(!menu_categories_hidden_path("GamesExtra/Test.iso"));
    assert(menu_category_toggle(0)==0);assert(!menu_support_entry("Games",1,1));
    assert(menu_categories_parse(custom,strlen(custom))==0);assert(menu_categories_count()==2);assert(menu_categories_hidden_path("Favorites 2/USA/Test.iso"));
    assert(!menu_categories_hidden_path("Games/Favorites 2/Test.iso"));assert(!menu_categories_hidden_path("/Storage/Games/Favorites 2/Test.iso"));
    strcpy(before,menu_categories_text());assert(menu_categories_parse("{}",2)<0);assert(!strcmp(before,menu_categories_text()));
    assert(menu_categories_parse(custom,strlen(custom)-1)<0);assert(!strcmp(before,menu_categories_text()));
    assert(menu_category_toggle(0)==0);assert(!menu_category_hidden(0));assert(strstr(menu_categories_text(),"\"defaults\":{\"Games\":\"games\"}"));
    assert(menu_category_toggle(0)==0);assert(menu_category_hidden(0));assert(!strcmp(before,menu_categories_text()));
    assert(menu_category_toggle(32)<0);puts("Categories: JSON bounds, custom names, hidden paths, root filtering and preserved configuration passed");return 0;
}
