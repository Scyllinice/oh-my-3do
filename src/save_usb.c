#include "save_usb.h"
#include <stdlib.h>
#include <string.h>

static int allowed(const char *path)
{
    size_t n = strlen(path);
    return !strcmp(path, "saves/recovery.3ds") || !strcmp(path, "saves/recovery2.3ds") ||
           (n == 16 && !strncmp(path, "saves/slot0", 11) && path[11] >= '1' && path[11] <= '8' && !strcmp(path + 12, ".3ds"));
}
int save_usb_load(Ode *ode, OdeReadBuffer reader, const char *path, SaveArchive *out)
{
    unsigned char header[SAVE_HEADER], *bytes = NULL;
    size_t size;
    int result = -1;
    if (!allowed(path) || usb_file_open(ode, path) < 0) return -1;
    if (usb_file_read(ode, reader, header, sizeof(header), 1) < 0 || memcmp(header, "OM3DSAV1", 8)) goto done;
    size = (size_t)save_word(header + 8);
    if (size < SAVE_HEADER || size > SAVE_MAX_BYTES) goto done;
    bytes = (unsigned char *)malloc(size);
    if (!bytes) goto done;
    memcpy(bytes, header, SAVE_HEADER);
    if (usb_file_read(ode, reader, bytes + SAVE_HEADER, size - SAVE_HEADER, 0) < 0) goto done;
    result = save_archive_parse(out, bytes, size);
done:
    free(bytes);
    if (usb_file_close(ode) < 0) result = -1;
    return result;
}
/* Full byte comparison, not just a command acknowledgement. File lengths are
 * self-described: FCare cannot truncate existing USB files. Ignore old tails. */
static int verify(Ode *ode, OdeReadBuffer reader, const char *path,
                  const unsigned char *bytes, size_t length)
{
    int result;
    if (usb_file_open(ode, path) < 0) return -1;
    result = usb_file_verify(ode, reader, bytes, length);
    if (usb_file_close(ode) < 0) result = -1;
    return result;
}

static int store(Ode *ode, OdeReadBuffer reader, const char *path,
                 const unsigned char *bytes, size_t length)
{
    int result;
    if (usb_file_open(ode, path) < 0) return -1;
    result = usb_file_write(ode, bytes, length);
    if (usb_file_close(ode) < 0) result = -1;
    if (result < 0) return -1;
    return verify(ode, reader, path, bytes, length);
}
int save_usb_store(Ode *ode, OdeReadBuffer reader, const char *path, const SaveArchive *a)
{
    if (!allowed(path) || !a->bytes || a->length < SAVE_HEADER) return -1;
    return store(ode, reader, path, a->bytes, a->length);
}
int save_usb_test(Ode *ode, OdeReadBuffer reader)
{
    unsigned char bytes[4099];
    size_t i;
    /* Two different patterns catch silent no-ops and stale prior test data. */
    int pass;
    for (pass = 0; pass < 2; ++pass) {
        for (i = 0; i < sizeof(bytes); ++i)
            bytes[i] = (unsigned char)((i * 37 + i / 2048 + 71 * pass + 19) & 255);
        if (store(ode, reader, "saves/usb-test.bin", bytes, sizeof(bytes)) < 0) return -1;
    }
    return 0;
}
