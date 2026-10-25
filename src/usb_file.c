/*
 * Files on the USB drive through the FCare's file commands. See usb_file.h
 * for the commands and the quirks this code works around.
 */
#include "usb_file.h"
#include <stdlib.h>
#include <string.h>

/* Command codes. */
#define CMD_OPEN_FILE         0xe1
#define CMD_READ_FILE         0xe3
#define CMD_WRITE_FILE        0xe4
#define CMD_CLOSE_FILE        0xe5
#define CMD_WRITE_BUFFER_WORD 0xe6

/* Status bit set in a reply's last byte when the command failed. */
#define STATUS_ERROR 0x10

/* The size of the FCare's data buffer, and of one read or write. */
#define BLOCK_BYTES 2048

/* The longest path the open command takes, including its zero byte. */
#define LONGEST_PATH 128

/* The size of a file header cleared before reading it (see USB_FILE_CRC_CHECKED). */
#define HEADER_BYTES 24

/* Short text is read in pieces of this size (see read_text_pieces). */
#define TEXT_PIECE 128

/*
 * Sends one file command and checks its reply: `expected` bytes, echoing the
 * command, without the error bit. A malformed reply means the command
 * channel may be out of step, so browsing stops (see ode.h). Returns 0, or -1.
 */
static int send_command(Ode *ode, unsigned char *packet, unsigned char *reply, size_t expected)
{
    size_t length = 0;

    if (ode->navigation_failed)
        return -1;
    if (ode->transfer(ode->context, packet, reply, 32, &length) < 0 || length != expected ||
        reply[0] != packet[0]) {
        ode->transport_uncertain = 1;
        ode->navigation_failed = 1;
        return -1;
    }
    if (reply[length - 1] & STATUS_ERROR)
        return -1;
    return 0;
}

/* Puts 4 bytes into the FCare's buffer at word `offset` (byte offset * 4). */
static int write_buffer_word(Ode *ode, unsigned int offset, const unsigned char *four_bytes)
{
    unsigned char packet[7], reply[32];
    packet[0] = CMD_WRITE_BUFFER_WORD;
    packet[1] = (unsigned char)(offset >> 8);
    packet[2] = (unsigned char)offset;
    memcpy(packet + 3, four_bytes, 4);
    return send_command(ode, packet, reply, 2);
}

/* Puts `size` bytes into the start of the FCare's buffer, 4 at a time (the
 * last word padded with zeros). */
static int write_buffer(Ode *ode, const unsigned char *data, size_t size)
{
    unsigned char word[4];
    size_t i;
    for (i = 0; i < size; i += 4) {
        size_t take = size - i > 4 ? 4 : size - i;
        memset(word, 0, sizeof(word));
        memcpy(word, data + i, take);
        if (write_buffer_word(ode, (unsigned int)(i / 4), word) < 0)
            return -1;
    }
    return 0;
}

/* Sets words `first` up to (not including) `last` of the buffer to zero. */
static int clear_buffer_words(Ode *ode, size_t first, size_t last)
{
    static const unsigned char zero[4] = {0, 0, 0, 0};
    size_t i;
    for (i = first; i < last; ++i) {
        if (write_buffer_word(ode, (unsigned int)i, zero) < 0)
            return -1;
    }
    return 0;
}

/*
 * Reads (or, for CMD_WRITE_FILE, writes) `size` bytes between the open file
 * and the FCare's buffer. The reply repeats the size on success.
 */
static int transfer_block(Ode *ode, unsigned char command, size_t size)
{
    unsigned char packet[7], reply[32];
    memset(packet, 0, sizeof(packet));
    packet[0] = command;
    packet[1] = (unsigned char)(size >> 8);
    packet[2] = (unsigned char)size;
    if (send_command(ode, packet, reply, 4) < 0 || reply[1] != packet[1] || reply[2] != packet[2])
        return -1;
    return 0;
}

/* Fills the FCare's buffer with the next `size` bytes of the open file. */
static int fill_buffer(Ode *ode, size_t size)
{
    return transfer_block(ode, CMD_READ_FILE, size);
}

/*
 * Copies the FCare's buffer to the console. A failure part-way leaves the
 * command channel in doubt, so browsing stops.
 */
static int fetch_buffer(Ode *ode, OdeReadBuffer reader, unsigned char *block)
{
    if (reader(ode->context, block, BLOCK_BYTES) < 0) {
        ode->transport_uncertain = 1;
        ode->navigation_failed = 1;
        return -1;
    }
    return 0;
}

int usb_file_open(Ode *ode, const char *name)
{
    unsigned char packet[7], reply[32];
    char path[LONGEST_PATH];
    const char *folder = ode->media_beside ? ode->media_folder : NULL;
    size_t length;

    if (ode->navigation_failed)
        return -1;

    /* Build "[folder/].ohmy3do/NAME". */
    if ((folder ? strlen(folder) + 1 : 0) + strlen(name) + sizeof(".ohmy3do/") > sizeof(path))
        return -1;
    path[0] = 0;
    if (folder) {
        strcpy(path, folder);
        strcat(path, "/");
    }
    strcat(path, ".ohmy3do/");
    strcat(path, name);
    length = strlen(path) + 1;

    /* These commands were only checked against the FCare. */
    if (strcmp(ode->identity, "LOCODE") || length > LONGEST_PATH)
        return -1;
    memcpy(ode->file_path, path, length); /* kept for diagnostics */

    /* The path goes into the buffer, then the open command gives its length.
     * The reply's second byte is nonzero when the file opened. */
    if (write_buffer(ode, (const unsigned char *)path, length) < 0)
        return -1;
    memset(packet, 0, sizeof(packet));
    packet[0] = CMD_OPEN_FILE;
    packet[2] = (unsigned char)(length >> 8);
    packet[3] = (unsigned char)length;
    if (send_command(ode, packet, reply, 3) < 0 || !reply[1])
        return -1;
    return 0;
}

int usb_file_close(Ode *ode)
{
    unsigned char packet[7] = {CMD_CLOSE_FILE, 0, 0, 0, 0, 0, 0};
    unsigned char reply[32];
    return send_command(ode, packet, reply, 2);
}

int usb_file_read(Ode *ode, OdeReadBuffer reader, unsigned char *out, size_t size, int header)
{
    unsigned char block[BLOCK_BYTES];

    while (size) {
        size_t chunk = size > sizeof(block) ? sizeof(block) : size;
        size_t words_to_clear;

        /*
         * Clear part of the buffer first (see USB_FILE_CRC_CHECKED): all of
         * a file's 24-byte header before the first read, or just the first
         * word before later reads, so a read past the end cannot return the
         * previous block unchanged. No seek or write commands are used.
         */
        if (header == USB_FILE_CRC_CHECKED)
            words_to_clear = 0;
        else if (header)
            words_to_clear = HEADER_BYTES / 4;
        else
            words_to_clear = 1;
        if (clear_buffer_words(ode, 0, words_to_clear) < 0)
            return -1;

        if (fill_buffer(ode, chunk) < 0)
            return -1;
        if (fetch_buffer(ode, reader, block) < 0)
            return -1;
        memcpy(out, block, chunk);
        if (ode->progress)
            ode->progress(ode->progress_context, chunk);
        out += chunk;
        size -= chunk;

        /* Only the very first read is a header. */
        if (header != USB_FILE_CRC_CHECKED)
            header = 0;
    }
    return 0;
}

int usb_file_read_block(Ode *ode, OdeReadBuffer reader, unsigned char *out)
{
    if (fill_buffer(ode, BLOCK_BYTES) < 0)
        return -1;
    return fetch_buffer(ode, reader, out);
}

int usb_file_probe(Ode *ode, OdeReadBuffer reader, unsigned char *out, size_t size,
                   unsigned char fill)
{
    unsigned char word[4];
    size_t i;

    if (size == 0 || size > HEADER_BYTES)
        return -1;
    memset(word, fill, sizeof(word));
    for (i = 0; i < (size + 3) / 4; ++i) {
        if (write_buffer_word(ode, (unsigned int)i, word) < 0)
            return -1;
    }
    return usb_file_read(ode, reader, out, size, USB_FILE_CRC_CHECKED);
}

int usb_file_write(Ode *ode, const unsigned char *data, size_t size)
{
    if (strcmp(ode->identity, "LOCODE"))
        return -1;

    /* A block at a time: fill the buffer, then write it to the file. (The
     * FCare's other ways to create or position files are not used.) */
    while (size) {
        size_t chunk = size > BLOCK_BYTES ? BLOCK_BYTES : size;
        if (write_buffer(ode, data, chunk) < 0)
            return -1;
        if (transfer_block(ode, CMD_WRITE_FILE, chunk) < 0)
            return -1;
        data += chunk;
        size -= chunk;
        if (ode->progress)
            ode->progress(ode->progress_context, chunk);
    }
    return 0;
}

int usb_file_verify(Ode *ode, OdeReadBuffer reader, const unsigned char *expected, size_t size)
{
    unsigned char block[BLOCK_BYTES], word[4];

    while (size) {
        size_t chunk = size > sizeof(block) ? sizeof(block) : size;
        size_t i, j;

        /*
         * Fill the buffer with the opposite of every expected byte first. If
         * the file is shorter than it should be, the read leaves those bytes
         * in place, and they can never match: a short file always fails.
         */
        for (i = 0; i < chunk; i += 4) {
            for (j = 0; j < 4; ++j)
                word[j] = i + j < chunk ? (unsigned char)(expected[i + j] ^ 255) : 0;
            if (write_buffer_word(ode, (unsigned int)(i / 4), word) < 0)
                return -1;
        }

        if (fill_buffer(ode, chunk) < 0)
            return -1;
        if (reader(ode->context, block, sizeof(block)) < 0) {
            ode->navigation_failed = 1;
            return -1;
        }
        if (memcmp(block, expected, chunk))
            return -1;
        if (ode->progress)
            ode->progress(ode->progress_context, chunk);
        expected += chunk;
        size -= chunk;
    }
    return 0;
}

/* The length of the text at the start of `block` (`size` bytes): up to its
 * first zero byte, or all of it. */
static size_t text_length_in(const unsigned char *block, size_t size)
{
    size_t length = 0;
    while (length < size && block[length])
        ++length;
    return length;
}

/*
 * Reads text in small pieces, each over a freshly cleared part of the
 * buffer, appending to `out` (which holds *used bytes so far) until *used
 * reaches `limit` or `capacity`. The text ends where a piece is not filled
 * completely. Returns 1 at the end of the text, 0 at the limit, or -1.
 */
static int read_text_pieces(Ode *ode, OdeReadBuffer reader, unsigned char *block, char *out,
                            size_t capacity, size_t limit, size_t *used)
{
    while (*used < capacity && *used < limit) {
        size_t piece = capacity - *used;
        size_t found;

        /*
         * Pieces of 128 bytes: short files are common at startup, and
         * clearing the whole 2 KiB buffer would take 512 commands before the
         * first byte arrived.
         */
        if (piece > TEXT_PIECE)
            piece = TEXT_PIECE;
        if (clear_buffer_words(ode, 0, (piece + 3) / 4) < 0)
            return -1;
        if (fill_buffer(ode, piece) < 0)
            return -1;
        if (reader(ode->context, block, BLOCK_BYTES) < 0) {
            ode->navigation_failed = 1;
            return -1;
        }

        found = text_length_in(block, piece);
        memcpy(out + *used, block, found);
        *used += found;
        if (found < piece)
            return 1;
    }
    return 0;
}

/* Whether all `size` bytes of `block` are zero. */
static int all_zero(const unsigned char *block, size_t size)
{
    size_t i;
    for (i = 0; i < size; ++i) {
        if (block[i])
            return 0;
    }
    return 1;
}

/*
 * Set once writing blank.bin has failed, so the menu stops trying (and
 * clears the buffer word by word instead).
 */
static int blank_file_unwritable;

/*
 * Clears the FCare's whole buffer in one go by reading .ohmy3do/blank.bin, a
 * file of 2048 zero bytes, into it. (The FCare has no "clear buffer"
 * command, and clearing it 4 bytes at a time takes 512 commands.) The menu
 * creates the file the first time it is needed, and checks the buffer really
 * came back all zeros every time, so a damaged file only means using the
 * slower way. Returns 1 if the buffer is now clear, 0 if not.
 */
static int clear_buffer_with_blank_file(Ode *ode, OdeReadBuffer reader, unsigned char *block)
{
    int attempt, clear = 0;

    /* First try reading the file; if that does not give zeros, write it
     * (with zeros) once and try again. */
    for (attempt = 0; attempt < 2 && !clear; ++attempt) {
        if (attempt > 0 && blank_file_unwritable)
            break;
        if (usb_file_open(ode, "blank.bin") < 0)
            break;
        if (attempt > 0) {
            memset(block, 0, BLOCK_BYTES);
            if (usb_file_write(ode, block, BLOCK_BYTES) < 0) {
                blank_file_unwritable = 1;
                usb_file_close(ode); /* already failing; the slow way still works */
                break;
            }
            if (usb_file_close(ode) < 0)
                break;
            if (usb_file_open(ode, "blank.bin") < 0)
                break;
        }
        if (fill_buffer(ode, BLOCK_BYTES) < 0) {
            usb_file_close(ode);
            break;
        }
        if (reader(ode->context, block, BLOCK_BYTES) < 0) {
            ode->navigation_failed = 1;
            usb_file_close(ode);
            break;
        }
        clear = all_zero(block, BLOCK_BYTES);
        if (usb_file_close(ode) < 0)
            return 0;
    }
    return clear;
}

/*
 * Text files have no length header, so the menu must find where the text
 * really ends despite the FCare's end-of-file behavior (a read at the end
 * still reports success and leaves the rest of the buffer stale):
 *
 *  1. Over a buffer cleared with blank.bin, one read finds the end of any
 *     text under 2 KiB (once the path the open command left at the start of
 *     the buffer is cleared too): it ends at the first zero. Without
 *     blank.bin, the first piece is read over words cleared one by one.
 *  2. Longer text is read in whole 2 KiB blocks, unchecked. A block is known
 *     to be complete once the next read brings different bytes: at the end
 *     of the file a read leaves the buffer exactly as it was.
 *  3. When a read repeats the block before it (or there is no room left),
 *     the text ends somewhere in that last block. Reopen the file, skip the
 *     complete blocks with reads alone, and read the last block over a
 *     buffer cleared from the point where it last differed from the block
 *     before it: everything before that point is certainly the file's.
 *  4. Anything still unknown is read in small cleared pieces, as in step 1.
 */
int usb_file_text(Ode *ode, OdeReadBuffer reader, const char *name, char *out, size_t capacity,
                  size_t *length)
{
    unsigned char block[BLOCK_BYTES];
    size_t used = 0; /* bytes of text known so far, in out */
    size_t clear_from = 0;
    size_t skip, i;
    int result = -1;
    int status = 0;   /* 1 once the end of the text is found */
    int pending = 0;  /* out holds one more block after `used`, not yet confirmed */
    int complete = 0; /* the block before the end is complete text */
    /* blank.bin lives in the drive's own .ohmy3do, not beside a game. */
    int buffer_clear =
        capacity && !ode->media_beside && clear_buffer_with_blank_file(ode, reader, block);
    const char *final_block = NULL; /* the block the text ends in, once known */

    if (!capacity || usb_file_open(ode, name) < 0)
        return -1;

    /* Step 1. */
    if (buffer_clear) {
        /* Opening left the path in the buffer's first words: clear those. */
        if (clear_buffer_words(ode, 0, (strlen(ode->file_path) + 4) / 4) < 0)
            goto done;
        if (fill_buffer(ode, sizeof(block)) < 0)
            goto done;
        if (reader(ode->context, block, sizeof(block)) < 0) {
            ode->navigation_failed = 1;
            goto done;
        }
        i = text_length_in(block, sizeof(block));
        if (i >= capacity)
            goto done;
        memcpy(out, block, i);
        used = i;
        if (i < sizeof(block))
            status = 1;
        else
            complete = 1; /* a whole block of text: compared against in step 3 */
    } else {
        status = read_text_pieces(ode, reader, block, out, capacity, TEXT_PIECE, &used);
    }

    /* Step 2: whole blocks while there is room. */
    while (!status && (pending || used + sizeof(block) < capacity)) {
        if (usb_file_read_block(ode, reader, block) < 0)
            goto done;

        if (pending) {
            /* The block read before this one is in `out` after `used`. */
            const char *zero = (const char *)memchr(out + used, 0, sizeof(block));
            if (!memcmp(block, out + used, sizeof(block))) {
                final_block = out + used; /* repeated: the end is in there */
                break;
            }
            if (zero) {
                used = (size_t)(zero - out); /* the text ended inside it */
                status = 1;
                break;
            }
            used += sizeof(block); /* it was complete text */
            complete = 1;
            pending = 0;
        }

        /* No room to keep this block: compare against it where it is. */
        if (used + sizeof(block) >= capacity) {
            final_block = (const char *)block;
            break;
        }
        memcpy(out + used, block, sizeof(block));
        pending = 1;
    }

    /* Step 3. */
    if (!status && final_block) {
        if (complete) {
            /* Find the last byte where the final block differs from the
             * complete block before it, rounded down to a word. */
            for (i = sizeof(block); i && final_block[i - 1] == out[used - sizeof(block) + i - 1];
                 --i) {
            }
            clear_from = i & ~(size_t)3;
        }
        if (usb_file_close(ode) < 0 || usb_file_open(ode, name) < 0)
            return -1;
        for (skip = used; skip; skip -= skip > sizeof(block) ? sizeof(block) : skip) {
            if (fill_buffer(ode, skip > sizeof(block) ? sizeof(block) : skip) < 0)
                goto done;
        }

        /* Clearing the tail is only cheaper than step 4 when it is the
         * smaller half of the buffer. */
        if (clear_from >= sizeof(block) / 2) {
            if (clear_buffer_words(ode, clear_from / 4, sizeof(block) / 4) < 0)
                goto done;
            if (fill_buffer(ode, sizeof(block)) < 0)
                goto done;
            if (reader(ode->context, block, sizeof(block)) < 0) {
                ode->navigation_failed = 1;
                goto done;
            }
            i = text_length_in(block, sizeof(block));
            if (i >= capacity - used)
                goto done;
            memcpy(out + used, block, i);
            used += i;
            if (i < sizeof(block))
                status = 1;
        }
    }

    /* Step 4. */
    if (!status)
        status = read_text_pieces(ode, reader, block, out, capacity, capacity, &used);

    if (status == 1) {
        out[used] = 0;
        *length = used;
        result = 0;
    }

done:
    if (usb_file_close(ode) < 0)
        result = -1;
    return result;
}

int usb_file_text_write(Ode *ode, OdeReadBuffer reader, const char *name, const char *text)
{
    size_t size = strlen(text) + 1; /* the zero byte marks the end on later reads */
    int result;

    if (usb_file_open(ode, name) < 0)
        return -1;
    result = usb_file_write(ode, (const unsigned char *)text, size);
    if (usb_file_close(ode) < 0)
        result = -1;

    if (result < 0 || usb_file_open(ode, name) < 0)
        return -1;
    result = usb_file_verify(ode, reader, (const unsigned char *)text, size);
    if (usb_file_close(ode) < 0)
        result = -1;
    return result;
}
