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

size_t menu_title_length(const char *name, int directory)
{
    size_t length = strlen(name);
    if (!directory && length > 4 &&
        (!menu_path_compare(name + length - 4, ".iso") || !menu_path_compare(name + length - 4, ".cue")))
        return length - 4;
    return length;
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

int menu_support_entry(const char *name, int directory, int root)
{
    static const char *files[] = {"boot.iso", "menu-demo.iso", "ohmy3do.ini", "favorites.txt",
        "boot.iso.sha256", "menu-demo.iso.sha256"};
    size_t i;
    if (!root) return 0;
    if (directory) return !menu_path_compare(name, "covers") || !menu_path_compare(name, "saves");
    for (i = 0; i < sizeof(files) / sizeof(files[0]); ++i)
        if (!menu_path_compare(name, files[i])) return 1;
    return 0;
}
