#include "save_catalog.h"
#include "usb_art.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct Fake {
    unsigned char files[32][SAVE_CATALOG_BYTES], buffer[2048];
    char names[32][48];
    size_t sizes[32], position;
    int file_count, current, opened, calls, fail_at, short_write, short_read, no_write, forbidden,
        writes;
} Fake;
static int fault(Fake *f)
{
    return ++f->calls == f->fail_at;
}
static int transfer(void *context, const unsigned char *p, unsigned char *r, size_t cap,
                    size_t *len)
{
    Fake *f = (Fake *)context;
    size_t n, off;
    if (fault(f))
        return -1;
    assert(cap >= 4);
    memset(r, 0, cap);
    r[0] = p[0];
    *len = 2;
    if (p[0] == 0xe6) {
        off = ((size_t)p[1] * 256 + p[2]) * 4;
        assert(off + 4 <= 2048);
        memcpy(f->buffer + off, p + 3, 4);
    } else if (p[0] == 0xe1) {
        const char *name = (const char *)f->buffer;
        assert(!strncmp(name, ".ohmy3do/", 9));
        name += 9;
        assert(!strncmp(name, "saves/", 6) || !strcmp(name, "ohmy3do.ini") ||
               !strcmp(name, "favorites.txt"));
        {
            int i;
            for (i = 0; i < f->file_count; ++i)
                if (!strcmp(f->names[i], name))
                    break;
            assert(i < 32);
            if (i == f->file_count)
                strcpy(f->names[f->file_count++], name);
            f->current = i;
        }
        f->position = 0;
        f->opened = 1;
        r[1] = 1;
        *len = 3;
    } else if (p[0] == 0xe4) {
        unsigned char *file = f->files[f->current];
        size_t *size = &f->sizes[f->current];
        ++f->writes;
        assert(f->opened);
        n = (size_t)p[1] * 256 + p[2];
        assert(n <= 2048);
        if (f->short_write && n)
            --n;
        if (!f->no_write) {
            assert(f->position + n <= sizeof(f->files[0]));
            memcpy(file + f->position, f->buffer, n);
            f->position += n;
            if (f->position > *size)
                *size = f->position;
        }
        /* Lie about short/failed writes just like the reference can. */
        r[1] = p[1];
        r[2] = p[2];
        *len = 4;
    } else if (p[0] == 0xe3) {
        const unsigned char *file = f->files[f->current];
        size_t size = f->sizes[f->current];
        assert(f->opened);
        n = (size_t)p[1] * 256 + p[2];
        assert(n <= 2048);
        if (n > size - f->position)
            n = size - f->position;
        if (f->short_read && n)
            --n;
        memcpy(f->buffer, file + f->position, n);
        f->position += n;
        r[1] = p[1];
        r[2] = p[2];
        *len = 4;
    } else if (p[0] == 0xe5)
        f->opened = 0;
    else {
        f->forbidden = 1;
        assert(0);
    }
    return 0;
}
static int reader(void *context, unsigned char *data, size_t size)
{
    Fake *f = (Fake *)context;
    if (fault(f))
        return -1;
    assert(size == 2048);
    memcpy(data, f->buffer, size);
    return 0;
}
static void setup(Fake *f, Ode *ode)
{
    memset(f, 0, sizeof(*f));
    memset(ode, 0, sizeof(*ode));
    ode->transfer = transfer;
    ode->context = f;
    strcpy(ode->identity, "LOCODE");
}

int main(void)
{
    static Fake f;
    Ode ode;
    SaveArchive archive = {0};
    SaveEntry entry;
    SaveNames names = {0};
    SaveCatalogResults results;
    SaveCatalogNode n, decoded;
    unsigned char bytes[SAVE_CATALOG_BYTES];
    int size, i, calls, at;
    {
        unsigned int slot = 99;
        assert(!save_catalog_slot_from_path("saves/slot01.3ds", &slot) && slot == 0);
        assert(!save_catalog_slot_from_path("saves/slot08.3ds", &slot) && slot == 7);
        assert(!save_catalog_slot_from_path("saves/slot10.3ds", &slot) && slot == 9);
        assert(!save_catalog_slot_from_path("saves/slot999999999.3ds", &slot) && slot == 999999998);
        assert(!save_catalog_slot_from_path("saves/recovery.3ds", &slot) &&
               slot == SAVE_RECOVERY_SLOT);
        assert(!save_catalog_slot_from_path("saves/recovery2.3ds", &slot) &&
               slot == SAVE_RECOVERY2_SLOT);
        assert(save_catalog_slot_from_path("saves/slot00.3ds", &slot) < 0);
        assert(save_catalog_slot_from_path("saves/slot001.3ds", &slot) < 0);
        assert(save_catalog_slot_from_path("saves/slot1.3ds", &slot) < 0);
        assert(save_catalog_slot_from_path("saves/slot1000000000.3ds", &slot) < 0);
    }
    memset(&entry, 0, sizeof(entry));
    strcpy(entry.name, "DoomPrefs");
    assert(!save_archive_build(&archive, &entry, 1));
    setup(&f, &ode);
    memset(&results, 0, sizeof(results));
    assert(save_catalog_search(&ode, reader, &names, "", 1, &results) < 0);
    assert(!save_catalog_update(&ode, reader, 999999998, &archive, "Last"));
    assert(!save_catalog_update(&ode, reader, 0, &archive, "First"));
    assert(!save_catalog_update(&ode, reader, 999, &archive, "Thousand"));
    assert(!save_catalog_update(&ode, reader, SAVE_RECOVERY_SLOT, &archive, ""));
    assert(!save_catalog_search(&ode, reader, &names, "doom", 0, &results) && results.total == 3 &&
           results.rows[0].slot == 999999998);
    assert(!save_catalog_search(&ode, reader, &names, "Thousand doom", 1, &results) &&
           results.total == 1 && results.rows[0].slot == 999);
    assert(!save_catalog_relabel(&ode, reader, 999, "Middle"));
    assert(!save_catalog_search(&ode, reader, &names, "thousand", 1, &results) && !results.total);
    assert(!save_catalog_search(&ode, reader, &names, "MIDDLE", 1, &results) && results.total == 1);
    assert(!save_catalog_search(&ode, reader, &names, "", 1, &results) && results.total == 4);
    results.offset = 2;
    assert(!save_catalog_search(&ode, reader, &names, "", 1, &results) && results.count == 2 &&
           results.rows[0].slot == 999);
    /* CRC protects the links as well as names; no partial results on corruption. */
    for (i = 0; i < f.file_count; ++i)
        if (!strcmp(f.names[i], "saves/slot1000.3si"))
            break;
    assert(i < f.file_count);
    f.files[i][16] ^= 1;
    results.offset = 0;
    assert(save_catalog_search(&ode, reader, &names, "", 1, &results) < 0 && !results.total &&
           !results.count);
    f.files[i][16] ^= 1;
    memset(&n, 0, sizeof(n));
    n.id = 1000;
    n.next = 1;
    n.state = 1;
    n.count = 1;
    strcpy(n.files[0], "icebreaker[levelchart]");
    size = save_catalog_encode(&n, bytes, sizeof(bytes), 0);
    assert(size == 100);
    assert(!save_catalog_decode(&decoded, bytes, (size_t)size, 0));
    assert(save_catalog_matches(&decoded, &names, "icebreaker"));
    assert(!save_catalog_matches(&decoded, &names, "doom"));
    n.next = 1000;
    assert(save_catalog_encode(&n, bytes, sizeof(bytes), 0) < 0);
    /* Reject every transport fault in a fresh catalog creation. */
    setup(&f, &ode);
    assert(!save_catalog_update(&ode, reader, 0, &archive, "First"));
    calls = f.calls;
    for (at = 1; at <= calls; ++at) {
        setup(&f, &ode);
        f.fail_at = at;
        assert(save_catalog_update(&ode, reader, 0, &archive, "First") < 0);
    }
    setup(&f, &ode);
    f.no_write = 1;
    assert(save_catalog_update(&ode, reader, 0, &archive, "First") < 0);
    /* Pagination retains all entries without scanning empty numerical slots. */
    setup(&f, &ode);
    for (i = 0; i < 12; ++i)
        assert(!save_catalog_update(&ode, reader, (unsigned int)i * 10000, &archive, ""));
    results.offset = 8;
    assert(!save_catalog_search(&ode, reader, &names, "doom", 0, &results) && results.total == 12 &&
           results.count == 4);
    /* Largest catalog record crosses the USB block boundary in both directions. */
    {
        SaveEntry many[SAVE_MAX_FILES];
        SaveArchive full = {0};
        memset(many, 0, sizeof(many));
        for (i = 0; i < SAVE_MAX_FILES; ++i)
            sprintf(many[i].name, "Game%03d", i);
        assert(!save_archive_build(&full, many, SAVE_MAX_FILES));
        setup(&f, &ode);
        assert(!save_catalog_update(&ode, reader, 0, &full, "Full"));
        results.offset = 0;
        assert(!save_catalog_search(&ode, reader, &names, "Game127", 0, &results) &&
               results.total == 1);
        save_archive_free(&full);
    }
    save_archive_free(&archive);
    puts("Save catalog: sparse slots, game/name search, paging, CRC and transport failures passed");
    return 0;
}
