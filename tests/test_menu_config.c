#include "menu_config.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct Fake {
    unsigned char buffer[2048];
    const char *file;
    const char *current_file;
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
        assert(!strcmp((const char *)fake->buffer,".ohmy3do/" MENU_CONFIG_NAME));
        assert(packet[1] == 0 && packet[2] == 0 && packet[3] == strlen((const char *)fake->buffer)+1);
        assert(!packet[4] && !packet[5] && !packet[6]);
        *length = 3;
        fake->current_file = fake->file;
        reply[1] = !fake->open_failure && fake->current_file != NULL;
        fake->opened = reply[1];
        break;
    case 0xe3:
        assert(fake->opened);
        assert(packet[1] == 2 && !packet[2]);
        /* Mimic FCare's requested-count response on a short read. The tail
         * is not cleared by the firmware; menu_config_load must do that. */
        for (offset = 0; offset < MENU_CONFIG_BYTES; ++offset)
            assert(fake->buffer[offset] == 0);
        memcpy(fake->buffer, fake->current_file, strlen(fake->current_file));
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
    assert(size == sizeof(fake->buffer) && fake->reads >= 1);
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

static void hide_adult(void)
{
    MenuConfig c;
    static const char full[] = "folder_workaround=0\nusb_covers=1\nscreen_margin=0\nlarge_titles=0\n"
        "remember_position=1\nrecent_games=1\nvideo_previews=0\npreview_sound=1\n" MENU_CONFIG_END "\n";
    assert(!menu_config_parse(&c,"",0) && !c.hide_adult);
    assert(!menu_config_parse(&c,"[library]\nhide_adult=1\n",23) && c.hide_adult);
    assert(menu_config_parse(&c,"hide_adult=2\n",13) < 0);
    assert(menu_config_parse(&c,"hide_adult=1\nhide_adult=0\n",26) < 0);
    /* A file saved before the setting existed still loads, with it off. */
    assert(!menu_config_parse(&c,full,strlen(full)) && !c.hide_adult);
}

static void parser(void)
{
    static const char *valid[] = {
        "", "; comment\n", "folder_workaround=0", "folder_workaround=1",
        "[compatibility]\r\n folder_workaround = 1 ; test\r\n",
        "\xef\xbb\xbf# BOM\nfolder_workaround=1 # yes\n"
    };
    static const char *invalid[] = {
        "folder_workaround=3", "folder_workaround=true", "folder_workaround=",
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
    /* 0 automatic (the shipped value), 1 always, 2 off. */
    assert(menu_config_parse(&config, "folder_workaround=2", 19) == 0 && config.folder_workaround == MENU_WORKAROUND_OFF);
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

static void readable_files(void)
{
    static const char settings[]="folder_workaround=1\r\nusb_covers=1\r\nvideo_previews=0\r\nscreen_margin=8\r\nlarge_titles=0\r\nremember_position=1\r\nrecent_games=1\r\npreview_sound=0\r\n";
    char data[MENU_CONFIG_BYTES+1];
    size_t used=sizeof(settings)-1;
    MenuConfig c;
    Fake fake; Ode ode;
    memset(data,' ',MENU_CONFIG_BYTES); data[MENU_CONFIG_BYTES]=0;
    memcpy(data,settings,used); memcpy(data+used,MENU_CONFIG_END "\r\n",sizeof(MENU_CONFIG_END)+1);
    assert(!menu_config_parse(&c,data,MENU_CONFIG_BYTES));
    assert(c.folder_workaround && c.screen_margin==8 && !c.preview_sound);
    /* Exact-size printable files go through the real startup USB loader. */
    setup(&fake,&ode); fake.file=data;
    assert(!menu_config_load(&ode,read_buffer,&c) && c.screen_margin==8 && !c.preview_sound);
    /* Text after the boundary cannot overwrite settings above it. */
    memcpy(data+used+sizeof(MENU_CONFIG_END)+1,"folder_workaround=0\r\n",21);
    assert(!menu_config_parse(&c,data,MENU_CONFIG_BYTES) && c.folder_workaround);
    /* PC edits may remove the blank tail entirely. */
    assert(!menu_config_parse(&c,data,used+sizeof(MENU_CONFIG_END)+1));
    memset(data+used,' ',MENU_CONFIG_BYTES-used);
    assert(menu_config_parse(&c,data,MENU_CONFIG_BYTES)<0); /* full buffer without a boundary */
    memcpy(data+used,MENU_CONFIG_END,sizeof(MENU_CONFIG_END)-2);
    assert(menu_config_parse(&c,data,MENU_CONFIG_BYTES)<0); /* cut-off boundary */
    assert(menu_config_parse(&c,MENU_CONFIG_END "\r\n",sizeof(MENU_CONFIG_END)+1)<0); /* incomplete generated settings */
    memcpy(data+used,MENU_CONFIG_END "\r\n",sizeof(MENU_CONFIG_END)+1);
    data[1]=0; assert(menu_config_parse(&c,data,MENU_CONFIG_BYTES)<0);
    assert(menu_config_parse(&c,data,MENU_CONFIG_BYTES+1)<0);
    puts("Readable INI: full-size USB load, end boundary, old-tail isolation and truncated/invalid files passed");
}

static void display_config(void)
{
    MenuConfig c;
    const char *valid="[compatibility]\nfolder_workaround=1\n[display]\nscreen_margin=12\nlarge_titles=1\n";
    assert(!menu_config_parse(&c,valid,strlen(valid)));
    assert(c.folder_workaround && c.screen_margin == 12 && c.large_titles && c.usb_covers);
    assert(menu_config_parse(&c,"screen_margin=13",16) < 0 && !c.folder_workaround && !c.screen_margin);
    assert(menu_config_parse(&c,"theme=3",7) < 0); /* Gecko TV is the only theme; the key is gone. */
    assert(menu_config_parse(&c,"[audio]\nmenu_music=1\n",strlen("[audio]\nmenu_music=1\n")) < 0); /* no background music */
    menu_config_defaults(&c);
    assert(!menu_config_parse(&c,"video_previews=0",strlen("video_previews=0")) && !c.video_previews);
    menu_config_defaults(&c);
    assert(c.remember_position && c.recent_games && c.video_previews && c.preview_sound);
    assert(!menu_config_parse(&c,"[audio]\npreview_sound=0\n",strlen("[audio]\npreview_sound=0\n")) && !c.preview_sound);
    assert(menu_config_parse(&c,"preview_sound=2",strlen("preview_sound=2")) < 0);
    assert(menu_config_parse(&c,"preview_sound=0\npreview_sound=1",strlen("preview_sound=0\npreview_sound=1")) < 0);
    assert(!menu_config_parse(&c,"[artwork]\nvideo_previews=1\n",strlen("[artwork]\nvideo_previews=1\n")) && c.video_previews);
    assert(menu_config_parse(&c,"video_previews=2",strlen("video_previews=2")) < 0);
    {
        const char *library="[library]\nremember_position=0\nrecent_games=0\n";
        assert(!menu_config_parse(&c,library,strlen(library)) && !c.remember_position && !c.recent_games);
        assert(menu_config_parse(&c,"remember_position=2",19) < 0);
    }
    assert(menu_config_parse(&c,"large_titles=2",14) < 0);
    assert(menu_config_parse(&c,"large_titles=1\nlarge_titles=0",strlen("large_titles=1\nlarge_titles=0")) < 0);
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
    hide_adult();
    default_template();
    display_config();
    readable_files();
    setup(&fake, &ode);
    assert(menu_config_load(&ode, read_buffer, &config) == 0);
    assert(config.folder_workaround && fake.closed == 1 && fake.reads == 1);
    setup(&fake,&ode); fake.file=NULL;
    assert(menu_config_load(&ode,read_buffer,&config)<0 && !fake.reads);
    setup(&fake,&ode); fake.file="invalid";
    assert(menu_config_load(&ode,read_buffer,&config)<0 && fake.reads==1);
    setup(&fake, &ode);
    fake.file = ""; /* missing files are opened/created empty by FCare */
    assert(menu_config_load(&ode, read_buffer, &config) == 0 && !config.folder_workaround);
    setup(&fake, &ode);
    strcpy(ode.identity, "OTHER!");
    assert(menu_config_load(&ode, read_buffer, &config) == 1);
    assert(!config.folder_workaround && !fake.calls);
    for (failed = 1; failed <= 137; ++failed) {
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
