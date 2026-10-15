#include "library_features.h"
#include "menu_ui.h"
#include <stdlib.h>
#include <string.h>

int menu_relative_path(const char *path)
{
    const char *p = path, *part = path;
    if (!*path || *path == '/' || strlen(path) >= FAVORITE_PATH) return 0;
    for (;; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c && (c < 32 || c == 127 || c == '\\' || c == ':')) return 0;
        if (!c || c == '/') {
            size_t n = (size_t)(p - part);
            if (!n || (n == 1 && *part == '.') || (n == 2 && !memcmp(part,"..",2))) return 0;
            if (!c) return 1;
            part = p + 1;
        }
    }
}
void menu_favorites_free(MenuFavorites *f)
{
    free(f->storage); memset(f, 0, sizeof(*f));
}
int menu_favorites_parse(MenuFavorites *f, const char *data, size_t size)
{
    MenuFavorites next;
    size_t pos = 0;
    memset(&next, 0, sizeof(next));
    if (size >= FAVORITES_BYTES || memchr(data, 0, size)) return -1;
    next.storage = (char *)malloc(size + 1);
    if (!next.storage) return -1;
    memcpy(next.storage, data, size); next.storage[size] = 0;
    if (size >= 3 && (unsigned char)data[0] == 0xef && (unsigned char)data[1] == 0xbb && (unsigned char)data[2] == 0xbf) pos = 3;
    while (pos < size) {
        size_t start = pos, end, i;
        while (pos < size && next.storage[pos] != '\n') ++pos;
        end = pos; if (pos < size) ++pos;
        while (start < end && (next.storage[start] == ' ' || next.storage[start] == '\t' || next.storage[start] == '\r')) ++start;
        while (end > start && (next.storage[end-1] == ' ' || next.storage[end-1] == '\t' || next.storage[end-1] == '\r')) --end;
        next.storage[end] = 0;
        if (start == end || next.storage[start] == '#' || next.storage[start] == ';') continue;
        if (next.count == FAVORITES_MAX || !menu_relative_path(next.storage + start)) goto bad;
        for (i = 0; i < next.count; ++i)
            if (!menu_path_compare(next.paths[i], next.storage + start)) goto bad;
        next.paths[next.count++] = next.storage + start;
    }
    menu_favorites_free(f); *f = next; return 0;
bad:
    menu_favorites_free(&next); return -1;
}
int menu_favorites_edit(MenuFavorites *f, const char *path, int remove)
{
    char *data;
    size_t i, found = f->count, used = 0;
    int result;
    if (!menu_relative_path(path) || *path == '#' || *path == ';' ||
        *path == ' ' || *path == '\t' || path[strlen(path)-1] == ' ') return -1;
    for (i = 0; i < f->count; ++i)
        if (!menu_path_compare(f->paths[i],path)) { found = i; break; }
    if (remove ? found == f->count : found != f->count) return 1;
    if (!remove && f->count == FAVORITES_MAX) return -1;
    data = (char *)malloc(FAVORITES_BYTES);
    if (!data) return -1;
    for (i = 0; i <= f->count; ++i) {
        const char *entry;
        size_t n;
        if (i == f->count) { if (remove) break; entry = path; }
        else { if (remove && i == found) continue; entry = f->paths[i]; }
        n = strlen(entry);
        if (used + n + 1 >= FAVORITES_BYTES) { free(data); return -1; }
        memcpy(data+used,entry,n); used += n; data[used++] = '\n';
    }
    data[used] = 0;
    result = menu_favorites_parse(f,data,used);
    free(data); return result;
}
int menu_favorites_format(const MenuFavorites *f, char *out, size_t capacity)
{
    size_t i, used = 0;
    if (!capacity) return -1;
    for (i = 0; i < f->count; ++i) {
        size_t n = strlen(f->paths[i]);
        if (used+n+1 >= capacity) return -1;
        memcpy(out+used,f->paths[i],n); used += n; out[used++] = '\n';
    }
    out[used] = 0; return 0;
}
int menu_favorites_move(MenuFavorites *f, size_t from, size_t to)
{
    const char *path;
    if (from >= f->count || to >= f->count) return -1;
    path = f->paths[from];
    if (from < to) memmove(f->paths+from,f->paths+from+1,(to-from)*sizeof(*f->paths));
    else if (from > to) memmove(f->paths+to+1,f->paths+to,(from-to)*sizeof(*f->paths));
    f->paths[to] = path; return 0;
}
int menu_recent_push(MenuFavorites *f, const char *path)
{
    char *data;
    size_t i, count = 1, used;
    int result;
    if (!menu_relative_path(path) || *path == '#' || *path == ';' || *path == ' ' || path[strlen(path)-1] == ' ') return -1;
    data = (char *)malloc(FAVORITES_BYTES);
    if (!data) return -1;
    used = strlen(path); memcpy(data,path,used); data[used++] = '\n';
    for (i = 0; i < f->count && count < 10; ++i) {
        size_t n = strlen(f->paths[i]);
        if (!menu_path_compare(path,f->paths[i])) continue;
        if (used+n+1 >= FAVORITES_BYTES) break;
        memcpy(data+used,f->paths[i],n); used += n; data[used++] = '\n'; ++count;
    }
    data[used] = 0; result = menu_favorites_parse(f,data,used); free(data); return result;
}
int menu_name_matches(const char *name, const char *query)
{
    const char *start;
    if (!*query) return 1;
    for (start = name; *start; ++start) {
        const char *a = start, *b = query;
        while (*a && *b) {
            unsigned char x = (unsigned char)*a, y = (unsigned char)*b;
            if (x >= 'a' && x <= 'z') x -= 'a'-'A';
            if (y >= 'a' && y <= 'z') y -= 'a'-'A';
            if (x != y) break;
            ++a; ++b;
        }
        if (!*b) return 1;
    }
    return 0;
}
int menu_initial(const char *name)
{
    unsigned char c = (unsigned char)*name;
    if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    return c >= 'A' && c <= 'Z' ? c - 'A' + 1 : 0;
}
static unsigned char lower(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + 'a' - 'A') : c;
}
static int separator(char c)
{
    return !c || c == ' ' || c == '_' || c == '-' || c == '(' || c == ')' || c == '[' || c == ']' || c == '.';
}
static int disc_key(const char *name, char *key)
{
    size_t n = strlen(name), i, number_start = 0, number_end = 0;
    int disc = 0, markers = 0;
    if (n >= ODE_MAX_NAME || n < 5 || (menu_path_compare(name + n - 4, ".iso") && menu_path_compare(name + n - 4, ".cue"))) return 0;
    n -= 4;
    for (i = 0; i < n; ++i) {
        size_t token = 0, j;
        int value = 0;
        if (i && !separator(name[i-1])) continue;
        if (i + 4 <= n && lower(name[i]) == 'd' && lower(name[i+1]) == 'i' && lower(name[i+2]) == 's' && (lower(name[i+3]) == 'c' || lower(name[i+3]) == 'k')) token = 4;
        else if (i + 2 <= n && lower(name[i]) == 'c' && lower(name[i+1]) == 'd') token = 2;
        if (!token) continue;
        j = i + token;
        while (j < n && (name[j] == ' ' || name[j] == '_' || name[j] == '-')) ++j;
        if (j == n || name[j] < '0' || name[j] > '9') continue;
        number_start = j;
        while (j < n && name[j] >= '0' && name[j] <= '9') {
            if (value > ODE_MAX_DISCS) return 0;
            value = value * 10 + name[j++] - '0';
        }
        if ((j < n && !separator(name[j])) || value < 1 || value > ODE_MAX_DISCS || ++markers > 1) return 0;
        disc = value; number_end = j;
    }
    if (markers != 1) return 0;
    memcpy(key, name, number_start);
    memcpy(key + number_start, name + number_end, n - number_end);
    key[number_start + n - number_end] = 0;
    return disc;
}
size_t menu_disc_group(const OdeListing *listing, const MenuEntry *selected,
                        const MenuEntry **out, size_t capacity)
{
    char key[ODE_MAX_NAME], other[ODE_MAX_NAME];
    const MenuEntry *discs[ODE_MAX_DISCS];
    size_t i, count = 0, highest = 0;
    if (selected->is_directory || disc_key(selected->name, key) != 1) return 0;
    memset(discs, 0, sizeof(discs));
    for (i = 0; i < listing->count; ++i) {
        const MenuEntry *entry = listing->entries[i];
        int number;
        if (entry->is_directory) continue;
        number = disc_key(entry->name, other);
        if (!number || menu_path_compare(key, other)) continue;
        if (discs[number - 1]) return 0;
        discs[number - 1] = entry; ++count;
        if ((size_t)number > highest) highest = (size_t)number;
    }
    if (count < 2 || count != highest || count > capacity) return 0;
    for (i = 0; i < count; ++i) out[i] = discs[i];
    return count;
}
