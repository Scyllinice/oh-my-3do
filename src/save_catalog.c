/*
 * The backup slot catalog: a sorted linked list of small node files.
 * See save_catalog.h for the layout.
 */
#include "save_catalog.h"
#include "usb_art.h"
#include <stdlib.h>
#include <string.h>

/* Node field offsets (see save_catalog.h). */
#define NODE_SIZE        8
#define NODE_ID          12
#define NODE_NEXT        16 /* slot nodes */
#define NODE_RESERVED    16 /* the start of the list */
#define NODE_CRC         20
#define NODE_FIRST       24 /* the start of the list: the first node's id */
#define NODE_STATE       24
#define NODE_LENGTH      28
#define NODE_ARCHIVE_CRC 32
#define NODE_LABEL       36
#define NODE_FILE_COUNT  64
#define NODE_FILES       68

/* Sizes: the start of the list, the smallest slot node, one file name. */
#define ROOT_NODE_BYTES 28
#define SLOT_NODE_BYTES 68
#define FILE_NAME_BYTES 32
#define LABEL_CHARS     24

/* The fill bytes used to detect an empty file (see save_usb.c). */
#define FIRST_SENTINEL  0xa5
#define SECOND_SENTINEL 0x5a

/* Matches shown per page of search results. */
#define RESULTS_PER_PAGE 8

static int is_valid_id(unsigned long id)
{
    return id > 0 && id <= SAVE_CATALOG_RECOVERY2;
}

unsigned long save_catalog_id(unsigned int slot)
{
    if (slot == SAVE_RECOVERY_SLOT)
        return SAVE_CATALOG_RECOVERY;
    if (slot == SAVE_RECOVERY2_SLOT)
        return SAVE_CATALOG_RECOVERY2;
    return (unsigned long)slot + 1; /* slot index 0 is "slot01" */
}

/* The slot index for a catalog id: the reverse of save_catalog_id. */
static unsigned int slot_for_id(unsigned long id)
{
    if (id == SAVE_CATALOG_RECOVERY)
        return (unsigned int)SAVE_RECOVERY_SLOT;
    if (id == SAVE_CATALOG_RECOVERY2)
        return (unsigned int)SAVE_RECOVERY2_SLOT;
    return (unsigned int)(id - 1);
}

int save_catalog_slot_from_path(const char *path, unsigned int *slot)
{
    unsigned long number = 0;
    size_t digits = 0;
    const char *p;

    if (!strcmp(path, "saves/recovery.3ds")) {
        *slot = SAVE_RECOVERY_SLOT;
        return 0;
    }
    if (!strcmp(path, "saves/recovery2.3ds")) {
        *slot = SAVE_RECOVERY2_SLOT;
        return 0;
    }

    /* "saves/slot" + the number (at least two digits, no extra leading
     * zero, as in save_usb.c) + ".3ds". */
    if (strncmp(path, "saves/slot", 10))
        return -1;
    for (p = path + 10; *p >= '0' && *p <= '9'; ++p) {
        if (++digits > 9)
            return -1;
        number = number * 10 + (unsigned long)(*p - '0');
    }
    if (digits < 2 || number == 0 || number > SAVE_SLOT_LIMIT || strcmp(p, ".3ds") ||
        (digits > 2 && path[10] == '0'))
        return -1;
    *slot = (unsigned int)(number - 1);
    return 0;
}

void save_catalog_path(char *path, unsigned long id)
{
    char reversed[12]; /* the id's digits, least significant first */
    size_t count = 0, position;

    if (id == 0) {
        strcpy(path, SAVE_CATALOG_ROOT);
        return;
    }
    if (id == SAVE_CATALOG_RECOVERY) {
        strcpy(path, "saves/recovery.3si");
        return;
    }
    if (id == SAVE_CATALOG_RECOVERY2) {
        strcpy(path, "saves/recovery2.3si");
        return;
    }

    /* "saves/slot" + the id with at least two digits + ".3si". */
    strcpy(path, "saves/slot");
    position = strlen(path);
    do {
        reversed[count++] = (char)('0' + id % 10);
        id /= 10;
    } while (id);
    if (count < 2)
        path[position++] = '0';
    while (count)
        path[position++] = reversed[--count];
    strcpy(path + position, ".3si");
}

/* Whether a string (up to its zero byte) is all printable ASCII. */
static int is_printable(const char *text)
{
    for (; *text; ++text) {
        unsigned char c = (unsigned char)*text;
        if (c < 32 || c > 126)
            return 0;
    }
    return 1;
}

/* Whether a node's contents make sense (`root` for the start of the list). */
static int is_valid_node(const SaveCatalogNode *node, int root)
{
    size_t i, j;

    if (root)
        return node->id == 0 && (node->next == 0 || is_valid_id(node->next));

    /* The list runs from higher ids to lower ones, so `next` must be lower. */
    if (!is_valid_id(node->id) || node->next >= node->id)
        return 0;
    if (node->state != SAVE_CATALOG_VALID && node->state != SAVE_CATALOG_INVALID)
        return 0;
    if (node->count > SAVE_MAX_FILES || !memchr(node->label, 0, LABEL_CHARS + 1))
        return 0;
    /* An invalid archive has no files to list. */
    if (node->state == SAVE_CATALOG_INVALID && node->count)
        return 0;
    if (!is_printable(node->label))
        return 0;

    for (i = 0; i < node->count; ++i) {
        const char *name = node->files[i];
        if (!name[0] || !memchr(name, 0, FILE_NAME_BYTES))
            return 0;
        for (j = 0; name[j]; ++j) {
            unsigned char c = (unsigned char)name[j];
            if (c < 32 || c == 127 || strchr("/\\:", c))
                return 0;
        }
    }
    return 1;
}

int save_catalog_decode(SaveCatalogNode *node, unsigned char *bytes, size_t length, int root)
{
    unsigned long stored_crc;
    size_t size;

    /* The magic, and a size that fits the kind of node. */
    if (length < 24 || memcmp(bytes, root ? "OM3DSLC1" : "OM3DSLT1", 8))
        return -1;
    size = (size_t)save_word(bytes + NODE_SIZE);
    if (size > length || size < ROOT_NODE_BYTES || size > SAVE_CATALOG_BYTES)
        return -1;
    if ((root && size != ROOT_NODE_BYTES) || (!root && size < SLOT_NODE_BYTES))
        return -1;

    /* The CRC was computed with its own field as zero: zero it, check, and
     * put the stored value back. */
    stored_crc = save_word(bytes + NODE_CRC);
    save_put_word(bytes + NODE_CRC, 0);
    if (usb_art_crc32(bytes, size) != stored_crc) {
        save_put_word(bytes + NODE_CRC, stored_crc);
        return -1;
    }
    save_put_word(bytes + NODE_CRC, stored_crc);

    memset(node, 0, sizeof(*node));
    node->id = save_word(bytes + NODE_ID);
    if (root) {
        if (save_word(bytes + NODE_RESERVED))
            return -1;
        node->next = save_word(bytes + NODE_FIRST);
    } else {
        node->next = save_word(bytes + NODE_NEXT);
        node->state = save_word(bytes + NODE_STATE);
        node->length = save_word(bytes + NODE_LENGTH);
        node->archive_crc = save_word(bytes + NODE_ARCHIVE_CRC);
        memcpy(node->label, bytes + NODE_LABEL, sizeof(node->label));
        node->count = (size_t)save_word(bytes + NODE_FILE_COUNT);
        if (node->count > SAVE_MAX_FILES || size != SLOT_NODE_BYTES + node->count * FILE_NAME_BYTES)
            return -1;
        memcpy(node->files, bytes + NODE_FILES, node->count * FILE_NAME_BYTES);
    }
    return is_valid_node(node, root) ? 0 : -1;
}

int save_catalog_encode(const SaveCatalogNode *node, unsigned char *bytes, size_t capacity,
                        int root)
{
    size_t size = root ? ROOT_NODE_BYTES : SLOT_NODE_BYTES + node->count * FILE_NAME_BYTES;

    if (!is_valid_node(node, root) || size > capacity)
        return -1;

    memset(bytes, 0, size);
    memcpy(bytes, root ? "OM3DSLC1" : "OM3DSLT1", 8);
    save_put_word(bytes + NODE_SIZE, (unsigned long)size);
    save_put_word(bytes + NODE_ID, node->id);
    if (root) {
        save_put_word(bytes + NODE_FIRST, node->next);
    } else {
        save_put_word(bytes + NODE_NEXT, node->next);
        save_put_word(bytes + NODE_STATE, node->state);
        save_put_word(bytes + NODE_LENGTH, node->length);
        save_put_word(bytes + NODE_ARCHIVE_CRC, node->archive_crc);
        strcpy((char *)bytes + NODE_LABEL, node->label);
        save_put_word(bytes + NODE_FILE_COUNT, (unsigned long)node->count);
        memcpy(bytes + NODE_FILES, node->files, node->count * FILE_NAME_BYTES);
    }
    /* The CRC field is still zero here, as the reader expects. */
    save_put_word(bytes + NODE_CRC, usb_art_crc32(bytes, size));
    return (int)size;
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
 * Reads the node with catalog id `id` (0 for the start of the list). Returns
 * 0, 1 if the file is empty (or missing, since opening creates it), or -1 if
 * it is damaged, has the wrong id or cannot be read. An empty file is
 * detected with sentinel bytes, as in save_usb.c.
 */
static int read_node(Ode *ode, OdeReadBuffer reader, unsigned long id, SaveCatalogNode *node)
{
    unsigned char header[24];
    unsigned char *bytes = NULL;
    char path[48];
    size_t size;
    int result = -1;

    save_catalog_path(path, id);
    if (usb_file_open(ode, path) < 0)
        return -1;

    if (usb_file_probe(ode, reader, header, sizeof(header), FIRST_SENTINEL) < 0)
        goto done;
    if (all_bytes_are(header, sizeof(header), FIRST_SENTINEL)) {
        /* Empty, or a file that starts with sentinel bytes: try the other one. */
        if (usb_file_close(ode) < 0 || usb_file_open(ode, path) < 0)
            return -1;
        if (usb_file_probe(ode, reader, header, sizeof(header), SECOND_SENTINEL) < 0)
            goto done;
        if (all_bytes_are(header, sizeof(header), SECOND_SENTINEL))
            result = 1;
        goto done;
    }

    /* The header gives the node's size; read the rest. The node has its own
     * CRC, so the quicker unchecked read is enough (see usb_file.h). */
    size = (size_t)save_word(header + NODE_SIZE);
    if (size < ROOT_NODE_BYTES || size > SAVE_CATALOG_BYTES)
        goto done;
    bytes = (unsigned char *)malloc(size);
    if (!bytes)
        goto done;
    memcpy(bytes, header, sizeof(header));
    if (usb_file_read(ode, reader, bytes + sizeof(header), size - sizeof(header),
                      USB_FILE_CRC_CHECKED) < 0)
        goto done;
    result = save_catalog_decode(node, bytes, size, id == 0);
    if (result == 0 && node->id != id)
        result = -1;

done:
    free(bytes);
    if (usb_file_close(ode) < 0)
        result = -1;
    return result;
}

/* Writes a node file and reads it back to check. Returns 0, or -1. */
static int write_node(Ode *ode, OdeReadBuffer reader, const SaveCatalogNode *node, int root)
{
    unsigned char *bytes = (unsigned char *)malloc(SAVE_CATALOG_BYTES);
    char path[48];
    int size, result = -1;

    if (!bytes)
        return -1;
    size = save_catalog_encode(node, bytes, SAVE_CATALOG_BYTES, root);
    save_catalog_path(path, root ? 0 : node->id);
    if (size < 0 || usb_file_open(ode, path) < 0)
        goto done;

    result = usb_file_write(ode, bytes, (size_t)size);
    if (usb_file_close(ode) < 0)
        result = -1;
    if (result < 0 || usb_file_open(ode, path) < 0) {
        result = -1;
        goto done;
    }
    result = usb_file_verify(ode, reader, bytes, (size_t)size);
    if (usb_file_close(ode) < 0)
        result = -1;

done:
    free(bytes);
    return result;
}

/* Copies a label into a node, cut to LABEL_CHARS characters. */
static void set_label(SaveCatalogNode *node, const char *label)
{
    strncpy(node->label, label, LABEL_CHARS);
    node->label[LABEL_CHARS] = 0;
}

int save_catalog_update(Ode *ode, OdeReadBuffer reader, unsigned int slot,
                        const SaveArchive *archive, const char *label)
{
    /* A node is large (4 KiB of file names), so it goes on the heap. */
    SaveCatalogNode *node = (SaveCatalogNode *)calloc(1, sizeof(*node));
    unsigned long id = save_catalog_id(slot);
    unsigned long current, previous = 0;
    size_t i;
    int result = -1, read_result;

    if (!node || !archive || !archive->bytes || !is_valid_id(id))
        goto done;

    /*
     * Find where this id belongs: walk down the list from the highest id
     * until reaching an id no higher than ours. `previous` is the node
     * before that point (0 when it is the start of the list).
     */
    read_result = read_node(ode, reader, 0, node);
    if (read_result < 0)
        goto done;
    current = read_result == 1 ? 0 : node->next; /* an empty catalog is an empty list */
    while (current > id) {
        if (read_node(ode, reader, current, node))
            goto done;
        previous = current;
        current = node->next;
    }
    /* If this slot already has a node, the new one takes its place. */
    if (current == id) {
        if (read_node(ode, reader, id, node))
            goto done;
        current = node->next;
    }

    /* Write the slot's node, pointing on to the rest of the list. */
    memset(node, 0, sizeof(*node));
    node->id = id;
    node->next = current;
    node->state = SAVE_CATALOG_VALID;
    node->length = (unsigned long)archive->length;
    node->archive_crc = save_word(archive->bytes + 16); /* the archive header's CRC */
    node->count = archive->count;
    if (label)
        set_label(node, label);
    for (i = 0; i < node->count; ++i)
        memcpy(node->files[i], archive->entries[i].name, FILE_NAME_BYTES);
    if (write_node(ode, reader, node, 0))
        goto done;

    /* Then point the node before it (or the start of the list) at it. */
    if (previous) {
        if (read_node(ode, reader, previous, node))
            goto done;
        node->next = id;
    } else {
        memset(node, 0, sizeof(*node));
        node->next = id;
    }
    result = write_node(ode, reader, node, previous == 0);

done:
    free(node);
    return result;
}

int save_catalog_relabel(Ode *ode, OdeReadBuffer reader, unsigned int slot, const char *label)
{
    SaveCatalogNode *node = (SaveCatalogNode *)malloc(sizeof(*node));
    int result = -1;

    if (!node)
        return -1;
    if (read_node(ode, reader, save_catalog_id(slot), node) == 0) {
        set_label(node, label);
        result = write_node(ode, reader, node, 0);
    }
    free(node);
    return result;
}

static int to_lower_ascii(int c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

/* Whether `text` contains the first `length` characters of `word`, ignoring
 * case. A NULL text contains nothing. */
static int contains(const char *text, const char *word, size_t length)
{
    size_t i;
    if (!text)
        return 0;
    for (; *text; ++text) {
        for (i = 0;
             i < length && text[i] &&
             to_lower_ascii((unsigned char)text[i]) == to_lower_ascii((unsigned char)word[i]);
             ++i) {
        }
        if (i == length)
            return 1;
    }
    return 0;
}

int save_catalog_matches(const SaveCatalogNode *node, const SaveNames *names, const char *query)
{
    char path[48];
    size_t length, i;
    int matched;

    /* The archive's own file name ("saves/slot07.3ds") can be searched too. */
    save_catalog_path(path, node->id);
    strcpy(path + strlen(path) - 4, ".3ds");

    /* Every word of the query must match somewhere. */
    while (*query) {
        while (*query == ' ')
            ++query;
        length = 0;
        while (query[length] && query[length] != ' ')
            ++length;
        if (length == 0)
            break;

        matched = contains(node->label, query, length) || contains(path, query, length);
        for (i = 0; !matched && i < node->count; ++i)
            matched = contains(node->files[i], query, length) ||
                      contains(save_names_find(names, node->files[i]), query, length);
        if (!matched)
            return 0;
        query += length;
    }
    return 1;
}

/*
 * Picks the text shown beside a search result: preferably a game title that
 * contains the query, else any game title, else the first file name, else a
 * note about the slot.
 */
static const char *result_hint(const SaveCatalogNode *node, const SaveNames *names,
                               const char *query)
{
    const char *hint = NULL;
    size_t i;

    for (i = 0; i < node->count; ++i) {
        const char *title = save_names_find(names, node->files[i]);
        if (title && (!hint || contains(title, query, strlen(query))))
            hint = title;
        if (title && *query && contains(title, query, strlen(query)))
            break;
    }
    if (hint)
        return hint;
    if (node->count)
        return node->files[0];
    return node->state == SAVE_CATALOG_INVALID ? "Invalid backup - inspect on PC" : "Empty archive";
}

int save_catalog_search(Ode *ode, OdeReadBuffer reader, const SaveNames *names, const char *query,
                        int recovery, SaveCatalogResults *out)
{
    SaveCatalogNode *node = (SaveCatalogNode *)malloc(sizeof(*node));
    unsigned long current;
    int result = -1;

    out->count = 0;
    out->total = 0;
    if (!node)
        return -1;

    /* Walk the whole list, counting matches and keeping one page of them. */
    if (read_node(ode, reader, 0, node))
        goto done;
    current = node->next;
    while (current) {
        int is_recovery = current > SAVE_SLOT_LIMIT;
        if (read_node(ode, reader, current, node))
            goto done;
        if ((recovery || !is_recovery) && save_catalog_matches(node, names, query)) {
            if (out->total >= out->offset && out->count < RESULTS_PER_PAGE) {
                SaveCatalogMatch *match = &out->rows[out->count++];
                memset(match, 0, sizeof(*match));
                match->slot = slot_for_id(current);
                match->state = node->state;
                strcpy(match->label, node->label);
                strncpy(match->hint, result_hint(node, names, query), SAVE_TITLE_BYTES - 1);
            }
            ++out->total;
        }
        current = node->next;
    }
    result = 0;

done:
    free(node);
    if (result < 0) {
        out->count = 0;
        out->total = 0;
    }
    return result;
}
