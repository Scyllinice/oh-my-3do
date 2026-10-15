#include "menu_position.h"
#include <string.h>
static int component(const char *name, size_t n)
{
    size_t i;
    if (!n || n >= ODE_MAX_NAME || (n == 1 && *name == '.') || (n == 2 && !memcmp(name,"..",2))) return 0;
    for (i = 0; i < n; ++i) if ((unsigned char)name[i] < 32 || name[i] == 127 || name[i] == '/' || name[i] == '\\' || name[i] == ':') return 0;
    return 1;
}
static int valid(const MenuPosition *p)
{
    const char *part, *end;
    if (strcmp(p->folder,"/Storage")) {
        if (strncmp(p->folder,"/Storage/",9)) return 0;
        part = p->folder+9;
        for (;;) {
            end = strchr(part,'/');
            if (!component(part,end ? (size_t)(end-part) : strlen(part))) return 0;
            if (!end) break;
            part = end+1;
        }
    }
    return !*p->selected || component(p->selected,strlen(p->selected));
}
int menu_position_parse(MenuPosition *p, const char *text, size_t length)
{
    const char *folder, *selection, *end;
    size_t n;
    memset(p,0,sizeof(*p));
    if (!length) return 0;
    if (length >= POSITION_BYTES || memchr(text,0,length) || length < 17 || memcmp(text,"OH3DO_POSITION1\n",16)) return -1;
    folder = text+16; selection = (const char *)memchr(folder,'\n',length-16);
    if (!selection || (n=(size_t)(selection-folder)) >= sizeof(p->folder)) return -1;
    memcpy(p->folder,folder,n); ++selection;
    end = (const char *)memchr(selection,'\n',length-(size_t)(selection-text));
    if (!end || end != text+length-1 || (n=(size_t)(end-selection)) >= sizeof(p->selected)) goto bad;
    memcpy(p->selected,selection,n);
    if (valid(p)) return 0;
bad:
    memset(p,0,sizeof(*p)); return -1;
}
int menu_position_format(const MenuPosition *p, char *out, size_t capacity)
{
    if (!valid(p) || strlen(p->folder)+strlen(p->selected)+19 > capacity) return -1;
    strcpy(out,"OH3DO_POSITION1\n"); strcat(out,p->folder); strcat(out,"\n"); strcat(out,p->selected); strcat(out,"\n");
    return 0;
}
