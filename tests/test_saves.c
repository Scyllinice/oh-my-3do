#include "save_usb.h"
#include "usb_art.h"
#include "menu_settings.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Fake {
    unsigned char file[65536], test[5000], buffer[2048];
    size_t size, test_size, position;
    int is_test, opened, calls, fail_at, short_write, short_read, no_write;
    int forbidden;
} Fake;
static int fault(Fake *f) { return ++f->calls == f->fail_at; }
static int transfer(void *context, const unsigned char *p, unsigned char *r, size_t cap, size_t *len)
{
    Fake *f = (Fake *)context;
    size_t n, off;
    if (fault(f)) return -1;
    assert(cap >= 4); memset(r, 0, cap); r[0] = p[0]; *len = 2;
    if (p[0] == 0xe6) {
        off = ((size_t)p[1] * 256 + p[2]) * 4; assert(off + 4 <= 2048); memcpy(f->buffer + off, p + 3, 4);
    } else if (p[0] == 0xe1) {
        assert(!strncmp((char *)f->buffer, "saves/", 6) || !strcmp((char *)f->buffer,"ohmy3do.ini") || !strcmp((char *)f->buffer,"favorites.txt"));
        f->is_test = !strcmp((char *)f->buffer, "saves/usb-test.bin");
        f->position = 0; f->opened = 1; r[1] = 1; *len = 3;
    } else if (p[0] == 0xe4) {
        unsigned char *file = f->is_test ? f->test : f->file;
        size_t *size = f->is_test ? &f->test_size : &f->size;
        assert(f->opened); n = (size_t)p[1] * 256 + p[2]; assert(n <= 2048);
        if (f->short_write && n) --n;
        if (!f->no_write) {
            assert(f->position + n <= (f->is_test ? sizeof(f->test) : sizeof(f->file)));
            memcpy(file + f->position, f->buffer, n); f->position += n;
            if (f->position > *size) *size = f->position;
        }
        /* Lie about short/failed writes just like the reference can. */
        r[1] = p[1]; r[2] = p[2]; *len = 4;
    } else if (p[0] == 0xe3) {
        const unsigned char *file = f->is_test ? f->test : f->file;
        size_t size = f->is_test ? f->test_size : f->size;
        assert(f->opened); n = (size_t)p[1] * 256 + p[2]; assert(n <= 2048);
        if (n > size - f->position) n = size - f->position;
        if (f->short_read && n) --n;
        memcpy(f->buffer, file + f->position, n); f->position += n;
        r[1] = p[1]; r[2] = p[2]; *len = 4;
    } else if (p[0] == 0xe5) f->opened = 0;
    else { f->forbidden = 1; assert(0); }
    return 0;
}
static int reader(void *context, unsigned char *data, size_t size)
{
    Fake *f = (Fake *)context;
    if (fault(f)) return -1;
    assert(size == 2048); memcpy(data, f->buffer, size); return 0;
}
static void setup(Fake *f, Ode *ode)
{
    memset(f, 0, sizeof(*f)); memset(ode, 0, sizeof(*ode));
    ode->transfer = transfer; ode->context = f; strcpy(ode->identity, "LOCODE");
}
static void fix_crc(unsigned char *data, size_t size)
{
    save_put_word(data + 16, usb_art_crc32(data + 24, size - 24));
}
int main(void)
{
    SaveArchive a, loaded, one;
    SaveEntry entries[2];
    static unsigned char payload[7003];
    static Fake f;
    Ode ode;
    unsigned char *broken;
    size_t i;
    int calls, fault_at;
    memset(&a, 0, sizeof(a)); memset(&loaded, 0, sizeof(loaded)); memset(&one, 0, sizeof(one));
    memset(entries, 0, sizeof(entries));
    for (i = 0; i < sizeof(payload); ++i) payload[i] = (unsigned char)(i * 31 + i / 2048);
    strcpy(entries[0].name, "Road Rash"); entries[0].type = 0x33444f46UL;
    entries[0].size = sizeof(payload); entries[0].data = payload;
    strcpy(entries[1].name, "Empty Save"); entries[1].type = 0x12345678UL;
    assert(!save_archive_build(&a, entries, 2)); assert(a.count == 2);
    assert(a.entries[0].type == entries[0].type && !memcmp(a.entries[0].data, payload, sizeof(payload)));
    assert(save_restore_matches(&a, &a, &a.entries[0]));
    assert(!save_archive_build(&one, entries, 1));
    assert(!save_restore_matches(&a, &one, &a.entries[0])); /* unrelated save lost */
    assert(!save_archive_build(&loaded, entries, 2));
    loaded.entries[1].type ^= 1;
    assert(!save_restore_matches(&a, &loaded, &a.entries[0])); /* unrelated metadata changed */
    save_archive_free(&loaded);
    /* Every truncated length and every single-byte corruption is rejected. */
    for (i = 0; i < a.length; ++i) assert(save_archive_parse(&loaded, a.bytes, i) < 0);
    broken = (unsigned char *)malloc(a.length); assert(broken);
    for (i = 0; i < a.length; ++i) {
        memcpy(broken, a.bytes, a.length); broken[i] ^= 1;
        assert(save_archive_parse(&loaded, broken, a.length) < 0);
    }
    memcpy(broken, a.bytes, a.length); broken[24] = '/'; fix_crc(broken, a.length);
    assert(save_archive_parse(&loaded, broken, a.length) < 0);
    memcpy(broken, a.bytes, a.length); save_put_word(broken + 24 + 36, 0xffffffffUL); fix_crc(broken, a.length);
    assert(save_archive_parse(&loaded, broken, a.length) < 0);
    strcpy(entries[1].name, "ROAD RASH"); assert(save_archive_build(&loaded, entries, 2) < 0);
    strcpy(entries[1].name, "../bad"); assert(save_archive_build(&loaded, entries, 2) < 0);
    entries[0].size = SAVE_MAX_BYTES; assert(save_archive_build(&loaded, entries, 1) < 0);
    entries[0].size = sizeof(payload);
    assert(!save_archive_build(&one, entries, 1));
    setup(&f, &ode); assert(!save_usb_test(&ode, reader)); assert(!f.forbidden);
    setup(&f, &ode); f.no_write = 1; assert(save_usb_test(&ode, reader) < 0);
    setup(&f, &ode); f.short_write = 1; assert(save_usb_test(&ode, reader) < 0);
    setup(&f, &ode); assert(!save_usb_store(&ode, reader, "saves/slot01.3ds", &a)); calls = f.calls;
    assert(!save_usb_load(&ode, reader, "saves/slot01.3ds", &loaded));
    assert(loaded.length == a.length && !memcmp(loaded.bytes, a.bytes, a.length));
    /* Replacing a larger slot with a smaller self-delimiting archive is valid. */
    assert(!save_archive_build(&one, NULL, 0));
    assert(!save_usb_store(&ode, reader, "saves/slot01.3ds", &one));
    assert(f.size > one.length); assert(!save_usb_load(&ode, reader, "saves/slot01.3ds", &loaded)); assert(!loaded.count);
    for (fault_at = 1; fault_at <= calls; ++fault_at) {
        setup(&f, &ode); f.fail_at = fault_at;
        assert(save_usb_store(&ode, reader, "saves/slot01.3ds", &a) < 0);
        assert(!f.forbidden);
    }
    setup(&f, &ode); f.short_write = 1; assert(save_usb_store(&ode, reader, "saves/slot01.3ds", &a) < 0);
    setup(&f, &ode); f.short_read = 1; assert(save_usb_store(&ode, reader, "saves/slot01.3ds", &a) < 0);
    /* Even an all-zero repeated tail must not pass a lying EOF read. */
    memset(payload, 0, sizeof(payload)); assert(!save_archive_build(&a, entries, 1));
    setup(&f, &ode); memcpy(f.file, a.bytes, a.length - 1); f.size = a.length - 1; f.no_write = 1;
    assert(save_usb_store(&ode, reader, "saves/slot01.3ds", &a) < 0);
    setup(&f, &ode); strcpy(ode.identity, "OTHER!"); assert(save_usb_store(&ode, reader, "saves/slot01.3ds", &a) < 0); assert(!f.calls);
    assert(save_usb_load(&ode, reader, "saves/../boot.iso", &loaded) < 0); assert(!f.calls);
    {
        MenuConfig config, parsed;
        char text[8192]; size_t length;
        menu_config_defaults(&config); config.folder_workaround=1; config.usb_covers=0;
        config.screen_margin=12; config.large_titles=1; config.theme=2;
        setup(&f,&ode); assert(!menu_settings_save(&ode,reader,&config));
        assert(f.size == MENU_CONFIG_BYTES);
        assert(!menu_config_parse(&parsed,(char *)f.file,strlen((char *)f.file)));
        assert(parsed.folder_workaround == 1 && !parsed.usb_covers && parsed.screen_margin == 12 && parsed.large_titles && parsed.theme == 2);
        config.folder_workaround=0; config.usb_covers=1; config.remember_position=0; config.recent_games=0;
        setup(&f,&ode); assert(!menu_settings_save(&ode,reader,&config));
        assert(!menu_config_parse(&parsed,(char *)f.file,strlen((char *)f.file)));
        assert(!parsed.folder_workaround && parsed.usb_covers && parsed.screen_margin == 12 && parsed.large_titles && parsed.theme == 2 && !parsed.remember_position && !parsed.recent_games);
        setup(&f,&ode); f.short_write=1; assert(menu_settings_save(&ode,reader,&config) < 0);
        setup(&f,&ode); f.no_write=1; assert(menu_settings_save(&ode,reader,&config) < 0);
        setup(&f,&ode); memcpy(f.file,"Games/D.iso\n",12); f.size=12;
        memset(f.buffer,0xa5,sizeof(f.buffer));
        assert(!usb_file_text(&ode,reader,"favorites.txt",text,sizeof(text),&length) && length == 12 && !memcmp(text,f.file,12));
        setup(&f,&ode); memcpy(f.file,"Old/Longer Game.iso\n",20); f.size=20;
        assert(!usb_file_text_write(&ode,reader,"favorites.txt","New.iso\n"));
        assert(!usb_file_text(&ode,reader,"favorites.txt",text,sizeof(text),&length));
        assert(length == 8 && !strcmp(text,"New.iso\n") && f.size == 20);
        assert(!usb_file_text_write(&ode,reader,"favorites.txt",""));
        assert(!usb_file_text(&ode,reader,"favorites.txt",text,sizeof(text),&length) && !length);
        setup(&f,&ode); f.no_write=1;
        assert(usb_file_text_write(&ode,reader,"favorites.txt","New.iso\n") < 0);
        setup(&f,&ode); f.short_write=1;
        assert(usb_file_text_write(&ode,reader,"favorites.txt","New.iso\n") < 0);
        setup(&f,&ode); memset(f.file,'a',8192); f.size=8192;
        assert(usb_file_text(&ode,reader,"favorites.txt",text,sizeof(text),&length) < 0);
        setup(&f,&ode); f.size=0; memset(f.buffer,0xa5,sizeof(f.buffer));
        assert(!usb_file_text(&ode,reader,"favorites.txt",text,sizeof(text),&length) && !length);
    }
    save_archive_free(&a); save_archive_free(&loaded); save_archive_free(&one); free(broken);
    puts("Saves: archive validation, USB write/read verification, short I/O and every transport failure passed");
    return 0;
}
