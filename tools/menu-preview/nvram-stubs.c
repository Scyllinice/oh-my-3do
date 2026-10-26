/*
 * Host stand-ins for the console's save memory (NVRAM) in the menu preview
 * (see tools/preview-menu.py). The generated program includes this after
 * the menu's browsing code. A fixture holds three saves; reads and writes
 * are counted, and reads can be made to fail.
 */
static int preview_mounts;
static int preview_mount(void *context, const unsigned char *packet, unsigned char *reply,
                         size_t capacity, size_t *length)
{
    Menu *menu = (Menu *)context;
    if (packet[0] != 0xc5 || menu->preview_audio.status || capacity < 2)
        abort();
    ++preview_mounts;
    reply[0] = 0xc5;
    reply[1] = 0;
    *length = 2;
    return 0;
}
static void activate(Menu *menu)
{
    (void)menu;
}
static int preview_nvram_reads, preview_nvram_writes, preview_nvram_fail;
static int preview_nvram_fixture(SaveArchive *a)
{
    SaveEntry e[3];
    memset(e, 0, sizeof(e));
    strcpy(e[0].name, "EA_RRX");
    e[0].data = (const unsigned char *)"save";
    e[0].size = 4;
    e[0].type = 0x33444f46UL;
    strcpy(e[1].name, "NFS_Config");
    e[1].data = (const unsigned char *)"other";
    e[1].size = 5;
    strcpy(e[2].name, "SY-TETSU-1J");
    e[2].data = (const unsigned char *)"tetsujin";
    e[2].size = 8;
    return save_archive_build(a, e, 3);
}
int nvram_snapshot(SaveArchive *a, NvramInfo *info)
{
    ++preview_nvram_reads;
    info->free_bytes = 14000;
    info->used_bytes = 18000;
    if (preview_nvram_fail) {
        info->error = "Fixture NVRAM error";
        return -1;
    }
    return preview_nvram_fixture(a);
}
int nvram_preflight(const SaveEntry *entry, NvramInfo *info)
{
    (void)entry;
    (void)info;
    return 0;
}
int nvram_restore(const SaveEntry *entry, NvramInfo *info)
{
    (void)entry;
    (void)info;
    ++preview_nvram_writes;
    return 0;
}
