#include "menu_ui.h"
#include <string.h>

static unsigned char fold(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

int menu_path_compare(const char *a, const char *b)
{
    while (*a && fold((unsigned char)*a) == fold((unsigned char)*b)) { ++a; ++b; }
    return (int)fold((unsigned char)*a) - (int)fold((unsigned char)*b);
}
static int menu_path_compare_prefix(const char *path, const char *prefix)
{
    while (*prefix && fold((unsigned char)*path) == fold((unsigned char)*prefix)) { ++path; ++prefix; }
    return *prefix != 0;
}

const char *menu_image_format(const char *name, int directory)
{
    size_t length = strlen(name);
    if (!directory && length > 4) {
        if (!menu_path_compare(name + length - 4, ".iso")) return "ISO";
        if (!menu_path_compare(name + length - 4, ".cue")) return "CUE";
    }
    return NULL;
}

size_t menu_title_length(const char *name, int directory)
{
    return strlen(name) - (menu_image_format(name, directory) ? 4 : 0);
}

static size_t tag_open(const char *name,size_t end)
{
    size_t open;
    char closing,opening;
    if (end<4 || ((closing=name[end-1])!=')' && closing!=']')) return 0;
    opening=closing==')' ? '(' : '[';
    for (open=end-2;open>1 && name[open]!=opening;--open)
        if (name[open]==closing) return 0;
    return name[open]==opening && name[open-1]==' ' ? open : 0;
}
static int geographic_tag(const char *text,size_t length)
{
    static const char *const words[]={
        "USA","Europe","Japan","Korea","World","Asia","Australia","Canada",
        "Germany","France","Spain","Italy","Netherlands","Brazil","China","Taiwan",
        "UK","United Kingdom","United States","Russia","Sweden","Norway","Denmark",
        "Finland","Portugal","Scandinavia","Latin America","New Zealand","Hong Kong",
        "En","Ja","Fr","De","Es","It","Nl","Pt","Ko","Zh","Ru","Ar",
        "Sv","No","Da","Fi","Pl","Cs","Sk","Hu","He","El","Tr",
        "English","Japanese","French","German","Spanish","Italian","Dutch",
        "Portuguese","Korean","Chinese","Russian","Arabic"
    };
    size_t start=0,end,i,j;
    int found;
    if (!length) return 0;
    while (start<length) {
        while (start<length && text[start]==' ') ++start;
        for (end=start;end<length && text[end]!=',';++end) {}
        j=end;while (j>start && text[j-1]==' ') --j;
        found=0;
        for (i=0;i<sizeof(words)/sizeof(words[0]);++i) {
            size_t k;
            if (strlen(words[i])!=j-start) continue;
            for (k=0;k<j-start && fold((unsigned char)text[start+k])==fold((unsigned char)words[i][k]);++k) {}
            if (k==j-start) { found=1;break; }
        }
        if (!found || (end<length && end+1==length)) return 0;
        start=end+1;
    }
    return 1;
}
size_t menu_short_title_length(const char *name, int directory)
{
    size_t length = menu_title_length(name, directory), open;
    if (directory) return length;
    while ((open=tag_open(name,length))!=0) {
        if (!geographic_tag(name+open+1,length-open-2)) break;
        length = open - 1;
    }
    return length;
}
size_t menu_display_title(char *out,size_t capacity,const char *name,int directory)
{
    size_t length=menu_title_length(name,directory),tags=length,open,pos,used=0,i;
    if (!capacity) return 0;
    if (!directory) while ((open=tag_open(name,tags))!=0) tags=open-1;
    for (i=0;i<tags && used+1<capacity;++i) out[used++]=name[i];
    pos=tags;
    while (pos<length) {
        char closing=name[pos+1]=='(' ? ')' : ']';
        size_t end=pos+2;
        while (end<length && name[end]!=closing) ++end;
        if (end==length || !geographic_tag(name+pos+2,end-pos-2))
            for (i=pos;i<=end && i<length && used+1<capacity;++i) out[used++]=name[i];
        pos=end+1;
    }
    out[used]=0;return used;
}

size_t menu_scroll_offset(size_t length, size_t visible, unsigned long elapsed)
{
    unsigned long distance, cycle, phase;
    if (length <= visible) return 0;
    distance = (unsigned long)(length - visible);
    cycle = 1200UL + distance * 180UL + 1000UL;
    phase = elapsed % cycle;
    if (phase < 1200UL) return 0;
    phase = (phase - 1200UL) / 180UL;
    return phase > distance ? (size_t)distance : (size_t)phase;
}

void menu_breadcrumb(char *out, const char *parent, const char *leaf,
                     unsigned int depth)
{
    size_t n;
    if (!depth) { strcpy(out, "/ STORAGE"); return; }
    strcpy(out, depth > 1 ? "... / " : "/ ");
    if (depth > 1 && strlen(parent) + strlen(leaf) + 9 <= 34) {
        strcat(out, parent);
        strcat(out, " / ");
    }
    n = strlen(out);
    strncpy(out + n, leaf, 34 - n);
    out[34] = 0;
    if (strlen(leaf) > 34 - n) memcpy(out + 31, "...", 3);
}

const char *menu_cover_lookup(const char *path)
{
    return menu_cover_find(menu_covers, menu_cover_count, path);
}

const char *menu_cover_find(const MenuCover *covers, size_t count, const char *path)
{
    size_t lo = 0, hi = count;
    if (!strncmp(path, "/Storage/", 9)) path += 9;
    while (*path == '/') ++path;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int order = menu_path_compare(path, covers[mid].path);
        if (!order) return covers[mid].file;
        if (order < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}

static int hide_adult;
void menu_set_hide_adult(int hide) { hide_adult = hide; }
int menu_adult_path(const char *path)
{
    int volume = 0;
    if (!strncmp(path,"/Storage/",9)) { path += 9; volume = 1; }
    if (!menu_path_compare_prefix(path,"Adult/")) return 1;
    if (volume && (path = strchr(path,'/')) != NULL) return !menu_path_compare_prefix(path+1,"Adult/");
    return 0;
}
int menu_support_entry(const char *name, int directory, int root)
{
    static const char *files[] = {"boot.iso", "menu-demo.iso", "boot.iso.sha256", "menu-demo.iso.sha256"};
    size_t i;
    size_t length = strlen(name);
    if (!directory && length >= 4 && !menu_path_compare(name + length - 4, ".bin")) return 1;
    if (!root) return 0;
    if (directory) return !menu_path_compare(name,".ohmy3do") || menu_categories_hidden_name(name) || (hide_adult && !menu_path_compare(name,"Adult"));
    for (i = 0; i < sizeof(files) / sizeof(files[0]); ++i)
        if (!menu_path_compare(name, files[i])) return 1;
    return 0;
}
