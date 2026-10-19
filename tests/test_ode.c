#include "ode.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct FakeOde {
    unsigned char cache[ODE_TOC_BYTES];
    unsigned long first_cached;
    unsigned long last_cached;
    unsigned long requested_id;
    unsigned long changed_id;
    size_t total;
    int parent;
    int devices;
    int fault;
    int failed;
    int short_reply;
    int bad_status;
    int reject_queue;
    int wrong_echo;
    int queued;
    int cleared;
    int launched;
    int calls;
    int clear_calls;
    unsigned long queued_ids[ODE_MAX_DISCS];
    unsigned long queued_directories[ODE_MAX_DISCS];
    size_t queue_count;
    int reject_at;
    int lose_add_reply;
    int lose_clear_reply;
    unsigned char sequence[16];
    int sequence_length;
} FakeOde;

static unsigned long be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | p[3];
}

static void put32(unsigned char *p, unsigned long value)
{
    p[0] = (unsigned char)(value >> 24);
    p[1] = (unsigned char)(value >> 16);
    p[2] = (unsigned char)(value >> 8);
    p[3] = (unsigned char)value;
}

static size_t append(FakeOde *fake, size_t position, unsigned long flags,
                     unsigned long id, const char *name)
{
    size_t length = strlen(name) + 1;
    assert(position + 12 + length <= sizeof(fake->cache));
    put32(fake->cache + position, flags);
    put32(fake->cache + position + 4, id);
    put32(fake->cache + position + 8, (unsigned long)length);
    memcpy(fake->cache + position + 12, name, length);
    return position + 12 + length;
}

static void populate(FakeOde *fake, unsigned long index)
{
    size_t position = 0;
    unsigned long id;
    memset(fake->cache, 0xff, sizeof(fake->cache));
    fake->first_cached = index + 1;
    fake->last_cached = index;
    if (fake->parent && index == 0)
        position = append(fake, position, 2, 0x01ffffffUL, "..");
    if (fake->devices) {
        append(fake, 0, 2, 0x01000000UL, "USB 0");
        return;
    }
    for (id = index + 1; id <= fake->total; ++id) {
        char name[128];
        /* Include long names to cross both 16-byte chunks and batch bounds.
         * Numeric order is deliberately opposite to insertion order. */
        sprintf(name, "Game %lu with a long descriptive filename.iso",
                (unsigned long)fake->total - id + 1);
        position = append(fake, position, id % 5 == 0 ? 2 : 1,
                          0x01000000UL | id, name);
        fake->last_cached = id;
        if (position > sizeof(fake->cache) - (128 + 13)) break;
    }
    if (fake->fault == 1) put32(fake->cache, 77);
    if (fake->fault == 2) put32(fake->cache + 8, 0x7fffffffUL);
    if (fake->fault == 3) {
        put32(fake->cache, 1);
        put32(fake->cache + 4, 1);
        put32(fake->cache + 8, 3);
        memcpy(fake->cache + 12, "bad", 3);
    }
    if (fake->fault == 4 && index) {
        /* A firmware bug repeats a previously returned entry. */
        append(fake, 0, 1, 0x01000001UL, "Repeated.iso");
    }
}

static int transfer(void *context, const unsigned char *command,
                    unsigned char *reply, size_t capacity, size_t *length)
{
    FakeOde *fake = (FakeOde *)context;
    unsigned long id = be32(command + 1);
    unsigned int offset = ((unsigned int)command[5] << 8) | command[6];
    assert(capacity >= 18);
    ++fake->calls;
    if (fake->failed) return -1;
    memset(reply, 0, capacity);
    reply[0] = command[0];
    *length = 2;
    if (fake->sequence_length < 16)
        fake->sequence[fake->sequence_length++] = command[0];
    switch (command[0]) {
    case 0x93:
        *length = 12;
        reply[1] = 2;
        memcpy(reply + 2, "LOCODE", 6);
        reply[8] = reply[9] = 1;
        break;
    case 0xc0:
        fake->changed_id = id;
        break;
    case 0xc1:
        assert(offset <= 2032);
        assert((offset % 16) == 0);
        if (!offset) populate(fake, id);
        *length = 18;
        memcpy(reply + 1, fake->cache + offset, 16);
        break;
    case 0xc3:
        ++fake->clear_calls;
        fake->cleared = 1;
        fake->queued = 0;
        fake->queue_count = 0;
        break;
    case 0xc4:
        assert(fake->cleared);
        fake->requested_id = id;
        id &= 0xffffffUL;
        *length = 4;
        reply[1] = (unsigned char)(id >= fake->first_cached && id <= fake->last_cached);
        reply[2] = reply[1];
        if (fake->reject_queue || (fake->reject_at &&
            fake->queue_count + 1 == (size_t)fake->reject_at)) reply[2] = 0;
        if (reply[1] && reply[2]) {
            assert(fake->queue_count < ODE_MAX_DISCS);
            fake->queued_ids[fake->queue_count] = be32(command + 1);
            fake->queued_directories[fake->queue_count++] = fake->changed_id;
            fake->queued = 1;
        }
        break;
    case 0xc5:
        assert(fake->queued);
        fake->launched = 1;
        reply[1] = 0x10; /* Intentional media-change status. */
        break;
    default: assert(0);
    }
    if (fake->short_reply) --*length;
    if (fake->bad_status) reply[*length - 1] = 0x10;
    if (fake->wrong_echo) reply[0] ^= 1;
    if (fake->lose_add_reply && command[0] == 0xc4) return -1;
    if (fake->lose_clear_reply && command[0] == 0xc3) return -1;
    return 0;
}

static void setup(FakeOde *fake, Ode *ode, OdeListing *listing)
{
    memset(fake, 0, sizeof(*fake));
    memset(ode, 0, sizeof(*ode));
    ode->transfer = transfer;
    ode->context = fake;
    ode_listing_init(listing);
}

static void directory_and_launch(void)
{
    FakeOde fake;
    Ode ode;
    OdeListing listing;
    const MenuEntry *target = NULL;
    size_t i;
    int files = 0;
    setup(&fake, &ode, &listing);
    fake.total = 100;
    fake.parent = 1;
    assert(ode_identify(&ode) == 0);
    assert(!strcmp(ode.identity, "LOCODE"));
    assert(ode.revision[0] == 1 && ode.revision[1] == 1);
    assert(ode_change_directory(&ode, 0x01000009UL) == 0);
    assert(fake.changed_id == 0x01000009UL);
    assert(ode_read_directory(&ode, &listing) == 0);
    assert(listing.count == 100);
    assert(listing.has_parent && listing.parent_id == 0x01ffffffUL);
    assert(fake.last_cached == 100 && fake.first_cached == 101);
    assert(listing.entries[0]->is_directory);
    assert(strstr(listing.entries[0]->name, "Game 1 "));
    for (i = 0; i < listing.count; ++i) {
        const MenuEntry *entry = listing.entries[i];
        if (!entry->is_directory) files = 1;
        else assert(!files);
        if (entry->toc_id == 0x01000002UL) target = entry;
    }
    assert(target);
    assert(strstr(target->name, "Game 99 "));
    fake.sequence_length = 0;
    assert(ode_launch(&ode, target) == 0);
    assert(fake.requested_id == 0x01000002UL && fake.launched);
    assert(fake.sequence_length == 4);
    assert(fake.sequence[0] == 0xc1 && fake.sequence[1] == 0xc3 &&
           fake.sequence[2] == 0xc4 && fake.sequence[3] == 0xc5);
    fake.launched = 0;
    fake.reject_queue = 1;
    assert(ode_launch(&ode, target) < 0 && !fake.launched);
    assert(ode_change_directory(&ode, listing.parent_id) == 0);
    assert(fake.changed_id == 0x01ffffffUL);
    assert(ode_launch(&ode, listing.entries[0]) < 0);
    ode_listing_free(&listing);
    ode_listing_free(&listing);
}

static void edge_cases(void)
{
    FakeOde fake;
    Ode ode;
    OdeListing listing;
    int fault;
    setup(&fake, &ode, &listing);
    assert(ode_read_directory(&ode, &listing) == 0 && !listing.count);
    fake.parent = 1;
    assert(ode_read_directory(&ode, &listing) == 0 && listing.has_parent);
    fake.parent = 0;
    fake.devices = 1;
    assert(ode_read_directory(&ode, &listing) == 0 && listing.count == 1);
    assert(listing.entries[0]->toc_id == 0x01000000UL);
    fake.devices = 0;
    for (fault = 1; fault <= 4; ++fault) {
        fake.total = 100;
        fake.fault = fault;
        assert(ode_read_directory(&ode, &listing) < 0);
        assert(!listing.count && !listing.entries);
        assert(ode.error);
    }
    fake.fault = 0;
    fake.total = ODE_MAX_ENTRIES;
    assert(ode_read_directory(&ode, &listing) == 0);
    assert(listing.count == ODE_MAX_ENTRIES);
    fake.total = ODE_MAX_ENTRIES + 1;
    assert(ode_read_directory(&ode, &listing) < 0);
    assert(!listing.count && !listing.entries);
    fake.failed = 1;
    assert(ode_identify(&ode) < 0);
    assert(ode_read_directory(&ode, &listing) < 0 && !listing.entries);
    fake.failed = 0;
    fake.short_reply = 1;
    assert(ode_identify(&ode) < 0);
    fake.short_reply = 0;
    fake.wrong_echo = 1;
    assert(ode_read_directory(&ode, &listing) < 0 && !listing.entries);
    fake.wrong_echo = 0;
    fake.bad_status = 1;
    assert(ode_change_directory(&ode, 0) < 0);
    ode_listing_free(&listing);
}

static void multi_disc_playlist(void)
{
    FakeOde fake;
    Ode ode;
    OdeListing listing;
    OdePlaylist playlist;
    MenuEntry disc = { 0x01000002UL, 0, "Disc 1.iso" };
    int before;
    size_t i;
    setup(&fake, &ode, &listing);
    ode_playlist_init(&playlist);
    fake.total = 100;
    before = fake.calls;
    assert(ode_playlist_start(&ode, &playlist) < 0);
    assert(ode_playlist_add(&ode, &playlist, &disc, "/Game") < 0);
    assert(fake.calls == before);
    assert(ode_playlist_clear(&ode, &playlist) == 0);
    assert(playlist.synchronized && playlist.count == 0);
    assert(ode_playlist_start(&ode, &playlist) < 0 && !fake.launched);
    assert(ode_change_directory(&ode, 0x01000004UL) == 0);
    assert(ode_playlist_add(&ode, &playlist, &disc, "/Game/Disc 1") == 0);
    /* Names and directory context survive selection/listing changes. */
    disc.name = "Disc 2.iso";
    assert(!strcmp(playlist.paths[0], "/Game/Disc 1/Disc 1.iso"));
    assert(ode_change_directory(&ode, 0x01000007UL) == 0);
    /* The same local ID can refer to a different image in another folder. */
    assert(ode_playlist_add(&ode, &playlist, &disc, "/Game/Disc 2") == 0);
    disc.name = "Disc 10.iso";
    /* Use ID 88 (decimal), inside the simulated directory. */
    disc.toc_id = 0x01000000UL | 88;
    assert(ode_playlist_add(&ode, &playlist, &disc, "/Game/Disc 2") == 0);
    assert(playlist.count == 3 && fake.queue_count == 3);
    assert(fake.queued_ids[0] == 0x01000002UL && fake.queued_ids[1] == 0x01000002UL);
    assert(fake.queued_ids[2] == (0x01000000UL | 88));
    assert(fake.queued_directories[0] == 0x01000004UL);
    assert(fake.queued_directories[1] == 0x01000007UL);
    before = fake.calls;
    assert(ode_playlist_add(&ode, &playlist, &disc, "/Game/Disc 2") < 0);
    assert(fake.calls == before && playlist.count == 3);
    disc.is_directory = 1;
    assert(ode_playlist_add(&ode, &playlist, &disc, "/Game") < 0);
    assert(fake.calls == before);
    disc.is_directory = 0;
    fake.sequence_length = 0;
    assert(ode_playlist_start(&ode, &playlist) == 0);
    assert(fake.sequence_length == 1 && fake.sequence[0] == 0xc5);
    assert(fake.clear_calls == 1 && fake.launched);
    assert(!playlist.synchronized);
    before = fake.calls;
    assert(ode_playlist_start(&ode, &playlist) < 0 && fake.calls == before);
    assert(ode_playlist_clear(&ode, &playlist) == 0);
    assert(!playlist.count && !fake.queue_count && playlist.synchronized);
    /* Match the firmware's 16-disc capacity; overflow must not send a command. */
    for (i = 0; i < ODE_MAX_DISCS; ++i) {
        char directory[40];
        sprintf(directory, "/Game/Selection %lu", (unsigned long)i);
        assert(ode_playlist_add(&ode, &playlist, &disc, directory) == 0);
    }
    assert(playlist.count == ODE_MAX_DISCS);
    before = fake.calls;
    assert(ode_playlist_add(&ode, &playlist, &disc, "/Game/Overflow") < 0);
    assert(fake.calls == before);
    ode_playlist_dispose(&playlist);
    ode_playlist_dispose(&playlist);
}

static void playlist_failures(void)
{
    FakeOde fake;
    Ode ode;
    OdeListing listing;
    OdePlaylist playlist;
    MenuEntry disc = { 0x01000002UL, 0, "Disc 1.iso" };
    int before;
    setup(&fake, &ode, &listing);
    ode_playlist_init(&playlist);
    fake.total = 100;
    assert(ode_playlist_clear(&ode, &playlist) == 0);
    assert(ode_playlist_add(&ode, &playlist, &disc, "/Game") == 0);
    fake.reject_at = 2;
    disc.name = "Disc 2.iso";
    assert(ode_playlist_add(&ode, &playlist, &disc, "/Game") < 0);
    assert(playlist.count == 1 && fake.queue_count == 1 && playlist.synchronized);
    assert(!fake.launched);
    fake.reject_at = 0;
    fake.lose_add_reply = 1;
    assert(ode_playlist_add(&ode, &playlist, &disc, "/Game") < 0);
    /* Firmware appended it, but the menu never received the acknowledgement. */
    assert(playlist.count == 1 && fake.queue_count == 2 && !playlist.synchronized);
    before = fake.calls;
    assert(ode_playlist_start(&ode, &playlist) < 0);
    assert(ode_playlist_add(&ode, &playlist, &disc, "/Game") < 0);
    assert(fake.calls == before && !fake.launched);
    fake.lose_add_reply = 0;
    fake.lose_clear_reply = 1;
    assert(ode_playlist_clear(&ode, &playlist) < 0);
    assert(playlist.count == 1 && !playlist.synchronized);
    fake.lose_clear_reply = 0;
    assert(ode_playlist_clear(&ode, &playlist) == 0);
    assert(!playlist.count && playlist.synchronized);
    assert(ode_playlist_add(&ode, &playlist, &disc, "/Game") == 0);
    fake.failed = 1;
    assert(ode_playlist_start(&ode, &playlist) < 0 && !playlist.synchronized);
    before = fake.calls;
    assert(ode_playlist_start(&ode, &playlist) < 0 && fake.calls == before);
    ode_playlist_dispose(&playlist);
}

static void directory_probes(void)
{
    FakeOde fake; Ode ode; OdeListing listing;
    int before;
    setup(&fake,&ode,&listing); fake.total=100; fake.parent=1;
    assert(ode_probe_directory(&ode,0x01000005UL,"Game 96 with a long descriptive filename.iso")==1);
    assert(!fake.changed_id && !fake.queued && !fake.launched && fake.calls<10);
    assert(ode_probe_directory(&ode,0x01000005UL,"Renamed.iso")==0);
    assert(ode_probe_directory(&ode,0x02000005UL,"Game 96 with a long descriptive filename.iso")==0);
    assert(ode_probe_directory(&ode,0x01000002UL,"Game 99 with a long descriptive filename.iso")==0);
    before=fake.calls; assert(!ode_probe_directory(&ode,0x01000000UL,"Volume") && fake.calls==before);
    /* A first record may be '..'; do not mistake it for the requested folder. */
    fake.devices=1;
    assert(!ode_probe_directory(&ode,0x01000001UL,"USB 0")); fake.devices=0;
    fake.failed=1;
    assert(ode_probe_directory(&ode,0x01000005UL,"Game 96 with a long descriptive filename.iso")==-1);
    puts("Directory probes: bounded ID/name/type verification, parent entries, volume mismatch and I/O failure passed");
}

int main(void)
{
    directory_probes();
    directory_and_launch();
    edge_cases();
    multi_disc_playlist();
    playlist_failures();
    puts("ode: directory, single/multi-disc launch and failure tests passed");
    return 0;
}
