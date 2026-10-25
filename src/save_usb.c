/*
 * Save archives on the USB drive. See save_usb.h.
 */
#include "save_usb.h"
#include <stdlib.h>
#include <string.h>

/* "saves/slot" followed by the slot number and ".3ds". */
#define SLOT_PREFIX        "saves/slot"
#define SLOT_PREFIX_LENGTH 10
#define SLOT_MAX_DIGITS    9

/* Two different fill bytes for detecting an empty file (see load_archive). */
#define FIRST_SENTINEL  0xa5
#define SECOND_SENTINEL 0x5a

/* Header field: the archive's total size. */
#define HEADER_TOTAL_SIZE 8

/*
 * Whether `path` is one the menu may read or write: a recovery archive, or a
 * backup slot. Slot numbers are written with at least two digits (slot01 to
 * slot99) and no leading zero beyond that (slot100, not slot0100), so each
 * slot has exactly one file name.
 */
static int is_save_path(const char *path)
{
    const char *p;
    unsigned long slot = 0;
    size_t digits = 0;

    if (!strcmp(path, "saves/recovery.3ds") || !strcmp(path, "saves/recovery2.3ds"))
        return 1;
    if (strncmp(path, SLOT_PREFIX, SLOT_PREFIX_LENGTH))
        return 0;

    for (p = path + SLOT_PREFIX_LENGTH; *p >= '0' && *p <= '9'; ++p) {
        if (++digits > SLOT_MAX_DIGITS)
            return 0;
        slot = slot * 10 + (unsigned long)(*p - '0');
    }
    if (digits < 2 || slot == 0 || slot > SAVE_SLOT_LIMIT || strcmp(p, ".3ds"))
        return 0;
    /* Two digits may start with 0 ("slot07"); longer numbers may not. */
    return digits == 2 || path[SLOT_PREFIX_LENGTH] != '0';
}

/* Whether all `size` bytes of `bytes` equal `value`. */
static int all_bytes_are(const unsigned char *bytes, size_t size, unsigned char value)
{
    size_t i;
    for (i = 0; i < size; ++i) {
        if (bytes[i] != value)
            return 0;
    }
    return 1;
}

/*
 * Reads the archive at `path` into `out`. With `allow_empty`, an empty file
 * is reported as 1 instead of being an error. Returns 0, 1 or -1.
 */
static int load_archive(Ode *ode, OdeReadBuffer reader, const char *path, SaveArchive *out,
                        int allow_empty)
{
    unsigned char header[SAVE_HEADER];
    unsigned char *bytes = NULL;
    size_t size;
    int result = -1;

    if (!is_save_path(path) || usb_file_open(ode, path) < 0)
        return -1;

    if (allow_empty) {
        /*
         * Opening a missing file creates it empty, and reading past the end
         * of a file still "succeeds" while leaving the FCare's buffer as it
         * was. So fill the buffer with a marker byte first: if the header
         * still reads as nothing but markers, the file is empty.
         */
        if (usb_file_probe(ode, reader, header, sizeof(header), FIRST_SENTINEL) < 0)
            goto done;
        if (all_bytes_are(header, sizeof(header), FIRST_SENTINEL)) {
            /*
             * Or the file really starts with 24 marker bytes. Try again with
             * a different marker; a real file cannot match both.
             */
            if (usb_file_close(ode) < 0)
                return -1;
            if (usb_file_open(ode, path) < 0)
                return -1;
            if (usb_file_probe(ode, reader, header, sizeof(header), SECOND_SENTINEL) < 0)
                goto done;
            if (all_bytes_are(header, sizeof(header), SECOND_SENTINEL))
                result = 1; /* empty */
            /* Either way the file is not a valid archive (it starts with 0xa5s). */
            goto done;
        }
    } else if (usb_file_read(ode, reader, header, sizeof(header), 1) < 0) {
        goto done;
    }

    /* The header says how big the whole archive is; read the rest. */
    if (memcmp(header, "OM3DSAV1", 8))
        goto done;
    size = (size_t)save_word(header + HEADER_TOTAL_SIZE);
    if (size < SAVE_HEADER || size > SAVE_MAX_BYTES)
        goto done;
    bytes = (unsigned char *)malloc(size);
    if (!bytes)
        goto done;
    memcpy(bytes, header, SAVE_HEADER);
    if (usb_file_read(ode, reader, bytes + SAVE_HEADER, size - SAVE_HEADER, 0) < 0)
        goto done;
    result = save_archive_parse(out, bytes, size);

done:
    free(bytes);
    if (usb_file_close(ode) < 0)
        result = -1;
    if (result == 1)
        save_archive_free(out);
    return result;
}

int save_usb_load(Ode *ode, OdeReadBuffer reader, const char *path, SaveArchive *out)
{
    return load_archive(ode, reader, path, out, 0);
}

int save_usb_load_target(Ode *ode, OdeReadBuffer reader, const char *path, SaveArchive *out)
{
    return load_archive(ode, reader, path, out, 1);
}

/*
 * Reads a file back and compares every byte with what was written, rather
 * than trusting the write command's acknowledgement. Only the first `length`
 * bytes are compared: the FCare cannot shorten a file, so a smaller archive
 * written over a larger one leaves the old end behind, which is harmless
 * because an archive's header gives its own size.
 */
static int verify_file(Ode *ode, OdeReadBuffer reader, const char *path, const unsigned char *bytes,
                       size_t length)
{
    int result;
    if (usb_file_open(ode, path) < 0)
        return -1;
    result = usb_file_verify(ode, reader, bytes, length);
    if (usb_file_close(ode) < 0)
        result = -1;
    return result;
}

/* Writes `length` bytes to `path`, then checks them with verify_file. */
static int write_file(Ode *ode, OdeReadBuffer reader, const char *path, const unsigned char *bytes,
                      size_t length)
{
    int result;
    if (usb_file_open(ode, path) < 0)
        return -1;
    result = usb_file_write(ode, bytes, length);
    if (usb_file_close(ode) < 0)
        result = -1;
    if (result < 0)
        return -1;
    return verify_file(ode, reader, path, bytes, length);
}

int save_usb_store(Ode *ode, OdeReadBuffer reader, const char *path, const SaveArchive *archive)
{
    if (!is_save_path(path) || !archive->bytes || archive->length < SAVE_HEADER)
        return -1;
    return write_file(ode, reader, path, archive->bytes, archive->length);
}

int save_usb_test(Ode *ode, OdeReadBuffer reader)
{
    /* A little over two FCare buffers, so the test crosses buffer boundaries. */
    unsigned char bytes[4099];
    size_t i;
    int pass;

    /*
     * Two different patterns: the second catches a drive that silently
     * ignores writes, since the file would still hold the first pattern (or
     * one from an earlier test).
     */
    for (pass = 0; pass < 2; ++pass) {
        for (i = 0; i < sizeof(bytes); ++i)
            bytes[i] = (unsigned char)((i * 37 + i / 2048 + 71 * pass + 19) & 255);
        if (write_file(ode, reader, "saves/usb-test.bin", bytes, sizeof(bytes)) < 0)
            return -1;
    }
    return 0;
}

int save_usb_store_checked(Ode *ode, OdeReadBuffer reader, const char *path,
                           const SaveArchive *archive, const SaveArchive *expected)
{
    /* SaveArchive is large, so it goes on the heap rather than the stack. */
    SaveArchive *current = (SaveArchive *)calloc(1, sizeof(*current));
    int result, unchanged;

    if (!current)
        return -1;

    /* Is the destination still what the person reviewed? */
    result = save_usb_load_target(ode, reader, path, current);
    if (expected)
        unchanged = result == 0 && current->length == expected->length &&
                    !memcmp(current->bytes, expected->bytes, current->length);
    else
        unchanged = result == 1; /* it was empty and still is */
    save_archive_free(current);
    free(current);

    if (!unchanged)
        return -1;
    return save_usb_store(ode, reader, path, archive);
}
