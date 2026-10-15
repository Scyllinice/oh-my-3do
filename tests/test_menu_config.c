#include "menu_config.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct Fake {
    unsigned char buffer[2048];
    const char *file;
    int opened, closed, calls, reads;
    int fail_at, open_failure, read_failure, data_failure, close_failure;
} Fake;

static int transfer(void *context, const unsigned char *packet,
                    unsigned char *reply, size_t capacity, size_t *length)
{
    Fake *fake = (Fake *)context;
    unsigned int offset;
    assert(capacity >= 4);
    if (++fake->calls == fake->fail_at) return -1;
    memset(reply, 0, capacity);
    reply[0] = packet[0];
    *length = 2;
    switch (packet[0]) {
    case 0xe6:
        offset = (((unsigned int)packet[1] << 8) | packet[2]) * 4;
        assert(offset + 4 <= sizeof(fake->buffer));
        memcpy(fake->buffer + offset, packet + 3, 4);
        break;
    case 0xe1:
        assert(!strcmp((const char *)fake->buffer, MENU_CONFIG_NAME));
        assert(packet[1] == 0 && packet[2] == 0 && packet[3] == sizeof(MENU_CONFIG_NAME));
        assert(!packet[4] && !packet[5] && !packet[6]);
        *length = 3;
        reply[1] = !fake->open_failure;
        fake->opened = !fake->open_failure;
        break;
    case 0xe3:
        assert(fake->opened);
        assert(packet[1] == 2 && !packet[2]);
        /* Mimic FCare's requested-count response on a short read. The tail
         * is not cleared by the firmware; menu_config_load must do that. */
        for (offset = 0; offset < MENU_CONFIG_BYTES; ++offset)
            assert(fake->buffer[offset] == 0);
        memcpy(fake->buffer, fake->file, strlen(fake->file));
        *length = 4;
        reply[1] = fake->read_failure ? 0 : packet[1];
        reply[2] = packet[2];
        ++fake->reads;
        break;
    case 0xe5:
        ++fake->closed;
        if (fake->close_failure) return -1;
        break;
    default: assert(0);
    }
    return 0;
}

static int read_buffer(void *context, unsigned char *data, size_t size)
{
    Fake *fake = (Fake *)context;
    assert(size == sizeof(fake->buffer) && fake->reads == 1);
    if (fake->data_failure) return -1;
    memcpy(data, fake->buffer, size);
    return 0;
}

static void setup(Fake *fake, Ode *ode)
{
    memset(fake, 0, sizeof(*fake));
    memset(fake->buffer, 0xa5, sizeof(fake->buffer));
    memset(ode, 0, sizeof(*ode));
    strcpy(ode->identity, "LOCODE");
    ode->transfer = transfer;
    ode->context = fake;
    fake->file = "folder_workaround=1\r\n";
}

static void parser(void)
{
    static const char *valid[] = {
        "", "; comment\n", "folder_workaround=0", "folder_workaround=1",
        "[compatibility]\r\n folder_workaround = 1 ; test\r\n",
        "\xef\xbb\xbf# BOM\nfolder_workaround=1 # yes\n"
    };
    static const char *invalid[] = {
        "folder_workaround=2", "folder_workaround=true", "folder_workaround=",
        "folder_workaround=10", "folder_workaround=1junk",
        "folder_workaround=1\nfolder_workaround=0", "folder_workarond=1",
        "[wrong]\nfolder_workaround=1", "folder_workaround"
    };
    MenuConfig config;
    char huge[MENU_CONFIG_BYTES];
    size_t i;
    for (i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        assert(menu_config_parse(&config, valid[i], strlen(valid[i])) == 0);
        assert(config.folder_workaround == (i >= 3));
    }
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        config.folder_workaround = 1;
        assert(menu_config_parse(&config, invalid[i], strlen(invalid[i])) < 0);
        assert(!config.folder_workaround);
    }
    memset(huge, ' ', sizeof(huge));
    assert(menu_config_parse(&config, huge, sizeof(huge)) < 0);
    assert(menu_config_parse(&config, "folder_workaround=1\0junk", 23) < 0);
    {
        const char *settings="folder_workaround=1\n[artwork]\nusb_covers=0\n";
        assert(menu_config_parse(&config,settings,strlen(settings)) == 0);
        assert(config.folder_workaround && !config.usb_covers);
        assert(menu_config_parse(&config,"",0) == 0 && config.usb_covers);
        settings="usb_covers=1\nusb_covers=0";
        assert(menu_config_parse(&config,settings,strlen(settings)) < 0);
    }
}

static void display_config(void)
{
    MenuConfig c;
    const char *valid="[compatibility]\nfolder_workaround=1\n[display]\nscreen_margin=12\nlarge_titles=1\ntheme=2\n";
    assert(!menu_config_parse(&c,valid,strlen(valid)));
    assert(c.folder_workaround && c.screen_margin == 12 && c.large_titles && c.theme == 2 && c.usb_covers);
    assert(menu_config_parse(&c,"screen_margin=13",16) < 0 && !c.folder_workaround && !c.screen_margin);
    assert(menu_config_parse(&c,"theme=3",7) < 0);
    assert(c.remember_position && c.recent_games && !c.video_previews && c.menu_music);
    assert(!menu_config_parse(&c,"[audio]\nmenu_music=0\n",strlen("[audio]\nmenu_music=0\n")) && !c.menu_music);
    assert(menu_config_parse(&c,"menu_music=2",strlen("menu_music=2")) < 0);
    assert(menu_config_parse(&c,"menu_music=0\nmenu_music=1",strlen("menu_music=0\nmenu_music=1")) < 0);
    assert(!menu_config_parse(&c,"[artwork]\nvideo_previews=1\n",strlen("[artwork]\nvideo_previews=1\n")) && c.video_previews);
    assert(menu_config_parse(&c,"video_previews=2",strlen("video_previews=2")) < 0);
    {
        const char *library="[library]\nremember_position=0\nrecent_games=0\n";
        assert(!menu_config_parse(&c,library,strlen(library)) && !c.remember_position && !c.recent_games);
        assert(menu_config_parse(&c,"remember_position=2",19) < 0);
    }
    assert(menu_config_parse(&c,"large_titles=2",14) < 0);
    assert(menu_config_parse(&c,"theme=1\ntheme=2",15) < 0);
}

static void default_template(void)
{
    char data[MENU_CONFIG_BYTES];
    MenuConfig c;
    FILE *file = fopen("ohmy3do.ini","rb");
    size_t length;
    assert(file); length = fread(data,1,sizeof(data),file);
    assert(!ferror(file)); fclose(file);
    assert(length < MENU_CONFIG_BYTES && !menu_config_parse(&c,data,length));
    assert(!c.folder_workaround && c.usb_covers && c.remember_position && c.recent_games);
}

int main(void)
{
    Fake fake;
    Ode ode;
    MenuConfig config;
    int failed;
    parser();
    default_template();
    display_config();
    setup(&fake, &ode);
    assert(menu_config_load(&ode, read_buffer, &config) == 0);
    assert(config.folder_workaround && fake.closed == 1);
    setup(&fake, &ode);
    fake.file = ""; /* missing files are opened/created empty by FCare */
    assert(menu_config_load(&ode, read_buffer, &config) == 0 && !config.folder_workaround);
    setup(&fake, &ode);
    strcpy(ode.identity, "OTHER!");
    assert(menu_config_load(&ode, read_buffer, &config) == 1);
    assert(!config.folder_workaround && !fake.calls);
    for (failed = 1; failed <= 134; ++failed) {
        setup(&fake, &ode);
        fake.fail_at = failed;
        assert(menu_config_load(&ode, read_buffer, &config) < 0);
        assert(!config.folder_workaround);
    }
    setup(&fake, &ode);
    fake.open_failure = 1;
    assert(menu_config_load(&ode, read_buffer, &config) < 0 && !fake.closed);
    setup(&fake, &ode);
    fake.read_failure = 1;
    assert(menu_config_load(&ode, read_buffer, &config) < 0 && fake.closed);
    setup(&fake, &ode);
    fake.data_failure = 1;
    assert(menu_config_load(&ode, read_buffer, &config) < 0 && fake.closed);
    setup(&fake, &ode);
    fake.close_failure = 1;
    assert(menu_config_load(&ode, read_buffer, &config) < 0 && !config.folder_workaround);
    setup(&fake, &ode);
    fake.file = "folder_workaround=1\ninvalid";
    assert(menu_config_load(&ode, read_buffer, &config) < 0 && !config.folder_workaround);
    puts("menu_config: parser, USB commands, short reads and failures passed");
    return 0;
}
