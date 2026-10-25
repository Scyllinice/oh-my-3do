/*
 * Loading the save cheat database from the drive. See save_cheats.c for the
 * file layout.
 */
#include "save_cheats_usb.h"
#include "save_cheats.h"
#include <stdlib.h>
#include <string.h>

/* The database header, whose size field says how much more to read. */
#define DB_HEADER_BYTES   24
#define HEADER_TOTAL_SIZE 8

int save_cheats_usb_load(Ode *ode, OdeReadBuffer reader)
{
    unsigned char header[DB_HEADER_BYTES];
    unsigned char *bytes = NULL;
    size_t size;
    int installed;

    if (usb_file_open(ode, "save-cheats.3db") < 0)
        return 0;

    /* Read the header, then the rest of the file it describes. */
    if (usb_file_read(ode, reader, header, DB_HEADER_BYTES, 1) < 0 ||
        (memcmp(header, "OM3DCHT1", 8) && memcmp(header, "OM3DCHT2", 8)))
        goto failed;
    size = (size_t)save_word(header + HEADER_TOTAL_SIZE);
    if (size < DB_HEADER_BYTES || size > SAVE_CHEAT_DB_MAX_BYTES)
        goto failed;
    bytes = (unsigned char *)malloc(size);
    if (!bytes)
        goto failed;
    memcpy(bytes, header, DB_HEADER_BYTES);
    if (usb_file_read(ode, reader, bytes + DB_HEADER_BYTES, size - DB_HEADER_BYTES, 0) < 0)
        goto failed;

    /*
     * Close the file before installing anything: if closing fails the USB
     * link is in doubt, and the data read might not be trustworthy.
     */
    if (usb_file_close(ode) < 0) {
        free(bytes);
        return 0;
    }
    installed = save_cheats_load(bytes, size) == 0;
    free(bytes);
    return installed;

failed:
    free(bytes);
    usb_file_close(ode); /* already failing; the built-in profiles stay in use */
    return 0;
}
