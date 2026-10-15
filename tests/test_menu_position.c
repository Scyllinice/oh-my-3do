#include "menu_position.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    MenuPosition p, parsed;
    char data[POSITION_BYTES]; size_t n;
    memset(&p,0,sizeof(p)); strcpy(p.folder,"/Storage/USB A/Games/D"); strcpy(p.selected,"D (Disc 2).iso");
    assert(!menu_position_format(&p,data,sizeof(data)));
    assert(!menu_position_parse(&parsed,data,strlen(data)) && !strcmp(parsed.folder,p.folder) && !strcmp(parsed.selected,p.selected));
    strcpy(p.folder,"/Storage"); p.selected[0]=0;
    assert(!menu_position_format(&p,data,sizeof(data)) && !menu_position_parse(&parsed,data,strlen(data)));
    for(n=1;n<strlen(data);++n) assert(menu_position_parse(&parsed,data,n) < 0);
    assert(!menu_position_parse(&parsed,"",0) && !*parsed.folder);
    strcpy(p.folder,"/Storage/../bad"); assert(menu_position_format(&p,data,sizeof(data)) < 0);
    strcpy(p.folder,"/Storage/Games"); strcpy(p.selected,"bad\nname"); assert(menu_position_format(&p,data,sizeof(data)) < 0);
    p.selected[0]=0; assert(menu_position_format(&p,data,16) < 0);
    puts("Position: root/folder/selection roundtrip, truncation, traversal, controls and bounds passed");
    return 0;
}
