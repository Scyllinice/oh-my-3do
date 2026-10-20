#include "save_names.h"
#include "usb_art.h"
#include <stdlib.h>
#include <string.h>
#include "save_name_db.h"
static int fold(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int same(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i) if (fold((unsigned char)a[i]) != fold((unsigned char)b[i])) return 0;
    return 1;
}
static size_t lines(const char *text, size_t length)
{
    size_t i, n = 0;
    for (i = 0; i < length; ++i) if (text[i] == '\n') ++n;
    return n + 1;
}
/* Splits one buffer's lines in place; malformed lines are skipped. */
static void add(SaveNames *names, char *text, size_t length, int learned)
{
    char *line = text, *end = text + length;
    while (line < end) {
        char *next = (char *)memchr(line, '\n', (size_t)(end - line)), *tab;
        if (!next) next = end;
        *next = 0;
        if (next > line && next[-1] == '\r') next[-1] = 0;
        tab = strchr(line, '\t');
        if ((line[0] == '=' || line[0] == '*') && tab && tab > line + 1 && tab - line - 1 < SAVE_NAME_BYTES && tab[1]) {
            SaveNameEntry *e = &names->entries[names->count++];
            *tab = 0;
            e->key = line + 1; e->title = tab + 1; e->prefix = line[0] == '*'; e->learned = learned;
        }
        line = next + 1;
    }
}
int save_names_parse(SaveNames *names, const char *disc, size_t disc_length, const char *learned, size_t learned_length)
{
    memset(names, 0, sizeof(*names));
    if (disc_length > SAVE_NAMES_BYTES || learned_length > SAVE_NAMES_BYTES) return -1;
    names->storage = (char *)malloc(disc_length + learned_length + 2);
    names->entries = (SaveNameEntry *)malloc((lines(disc, disc_length) + lines(learned, learned_length)) * sizeof(SaveNameEntry));
    if (!names->storage || !names->entries) { save_names_free(names); return -1; }
    memcpy(names->storage, learned, learned_length); names->storage[learned_length] = 0;
    memcpy(names->storage + learned_length + 1, disc, disc_length); names->storage[learned_length + 1 + disc_length] = 0;
    add(names, names->storage, learned_length, 1);
    add(names, names->storage + learned_length + 1, disc_length, 0);
    return 0;
}
void save_names_free(SaveNames *names)
{
    free(names->storage); free(names->entries);
    memset(names, 0, sizeof(*names));
}
const char *save_names_find(const SaveNames *names, const char *file)
{
    const SaveNameEntry *best = NULL;
    size_t i, n = strlen(file), longest = 0;
    for (i = 0; i < names->count; ++i) {
        const SaveNameEntry *e = &names->entries[i];
        size_t k = strlen(e->key);
        if (e->prefix) {
            if (k <= n && k > longest && same(e->key, file, k)) { best = e; longest = k; }
        } else if (k == n && same(e->key, file, n)) {
            if (e->learned) return e->title;
            if (!best || best->prefix) { best = e; longest = n + 1; }
        }
    }
    for (i=0;i<sizeof(builtin_save_names)/sizeof(builtin_save_names[0]);++i) {
        const SaveNameEntry *e=&builtin_save_names[i];
        size_t k=strlen(e->key);
        if (!e->prefix && k==n && same(e->key,file,n)) {
            if (!best || best->prefix) best=e;
        } else if (e->prefix && k<=n && k>longest && same(e->key,file,k) && (!best || best->prefix)) {best=e;longest=k;}
    }
    return best ? best->title : NULL;
}
static char *hex(char *out, unsigned long value)
{
    int i;
    for (i = 7; i >= 0; --i) { *out++ = "0123456789abcdef"[(value >> (i * 4)) & 15]; }
    return out;
}
static char *number(char *out, unsigned long value)
{
    char digits[12];
    int n = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (n) *out++ = digits[--n];
    return out;
}
int save_snapshot_format(char *out, size_t capacity, const char *title, size_t title_length, const SaveArchive *nvram)
{
    char *p = out;
    size_t i;
    if (!title_length || title_length >= SAVE_TITLE_BYTES || capacity < 16 + SAVE_TITLE_BYTES + nvram->count * (SAVE_NAME_BYTES + 22)) return -1;
    for (i = 0; i < title_length; ++i) if ((unsigned char)title[i] < 32) return -1;
    memcpy(p, "OH3DO_SAVESNAP1\n", 16); p += 16;
    memcpy(p, title, title_length); p += title_length; *p++ = '\n';
    for (i = 0; i < nvram->count; ++i) {
        const SaveEntry *e = &nvram->entries[i];
        size_t n = strlen(e->name);
        memcpy(p, e->name, n); p += n; *p++ = '\t';
        p = number(p, (unsigned long)e->size); *p++ = '\t';
        p = hex(p, usb_art_crc32(e->data, e->size)); *p++ = '\n';
    }
    *p = 0;
    return 0;
}
/* The snapshot line for file, or NULL. */
static const char *snapshot_line(const char *body, const char *end, const char *file)
{
    size_t n = strlen(file);
    while (body < end) {
        const char *next = (const char *)memchr(body, '\n', (size_t)(end - body));
        if (!next) next = end;
        if ((size_t)(next - body) > n && body[n] == '\t' && !memcmp(body, file, n)) return body + n + 1;
        body = next + 1;
    }
    return NULL;
}
int save_snapshot_learn(const char *snapshot, size_t length, const SaveArchive *nvram, const SaveNames *names,
                        char *learned, size_t capacity)
{
    const char *title, *body, *end = snapshot + length;
    size_t i, title_length, used = strlen(learned);
    int added = 0;
    if (length < 18 || memcmp(snapshot, "OH3DO_SAVESNAP1\n", 16)) return -1;
    title = snapshot + 16;
    body = (const char *)memchr(title, '\n', (size_t)(end - title));
    if (!body || (title_length = (size_t)(body - title)) == 0 || title_length >= SAVE_TITLE_BYTES) return -1;
    ++body;
    for (i = 0; i < nvram->count; ++i) {
        const SaveEntry *e = &nvram->entries[i];
        const char *old = snapshot_line(body, end, e->name);
        char expected[24], *p = expected;
        size_t n = strlen(e->name);
        p = number(p, (unsigned long)e->size); *p++ = '\t'; p = hex(p, usb_art_crc32(e->data, e->size));
        if (old && end - old >= p - expected && !memcmp(old, expected, (size_t)(p - expected)) &&
            (old + (p - expected) == end || old[p - expected] == '\n')) continue;
        if (save_names_find(names, e->name)) continue;
        if (used + n + title_length + 4 > capacity) return added;
        learned[used++] = '='; memcpy(learned + used, e->name, n); used += n; learned[used++] = '\t';
        memcpy(learned + used, title, title_length); used += title_length; learned[used++] = '\n'; learned[used] = 0;
        ++added;
    }
    return added;
}

int save_slot_labels_parse(SaveSlotLabels *out, const char *text, size_t length)
{
    SaveSlotLabels next;
    unsigned int seen=0;
    size_t pos=0;
    memset(&next,0,sizeof(next));
    if (length>SAVE_SLOT_LABEL_FILE_BYTES) return -1;
    while (pos<length && text[pos]) {
        size_t start=pos,end,n,i;
        unsigned int slot;
        while (pos<length && text[pos] && text[pos]!='\n') ++pos;
        end=pos;
        if (end>start && text[end-1]=='\r') --end;
        if (pos<length && text[pos]=='\n') ++pos;
        if (end==start) continue;
        if (end-start<2 || text[start]<'1' || text[start]>'8' || text[start+1]!='\t') return -1;
        slot=(unsigned int)(text[start]-'1'); n=end-start-2;
        if ((seen&(1U<<slot)) || n>=SAVE_SLOT_LABEL_BYTES) return -1;
        for (i=0;i<n;++i) if ((unsigned char)text[start+2+i]<32 || (unsigned char)text[start+2+i]>126) return -1;
        seen|=1U<<slot;
        memcpy(next.labels[slot],text+start+2,n);
    }
    *out=next; return 0;
}
int save_slot_labels_format(const SaveSlotLabels *labels, char *out, size_t capacity)
{
    size_t used=0,i,j;
    for (i=0;i<8;++i) if (labels->labels[i][0]) {
        size_t n=0;
        while (n<SAVE_SLOT_LABEL_BYTES && labels->labels[i][n]) ++n;
        if (n==SAVE_SLOT_LABEL_BYTES || used+n+4>capacity) return -1;
        for (j=0;j<n;++j) if ((unsigned char)labels->labels[i][j]<32 || (unsigned char)labels->labels[i][j]>126) return -1;
        out[used++]=(char)('1'+i); out[used++]='\t'; memcpy(out+used,labels->labels[i],n); used+=n; out[used++]='\n';
    }
    if (used>=capacity) return -1;
    out[used]=0; return 0;
}
