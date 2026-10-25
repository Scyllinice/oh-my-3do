/*
 * The backup slot catalog: a summary of every save archive on the drive
 * (its label and the save files inside), so the menu can list and search
 * backup slots without opening each archive.
 *
 * The catalog is a linked list kept in small files in .ohmy3do/saves:
 *
 *   slot-catalog.3di      the start of the list: points to the highest id
 *   slot01.3si, ...       one node per slot, each pointing to the next lower
 *   recovery.3si, ...     id in use (0 ends the list)
 *
 * Each slot has an id: its slot number (counting from 1), or the special
 * ids for the two recovery archives, which are highest so they come first.
 * Keeping the list sorted lets one slot be added or changed by rewriting at
 * most two small files.
 *
 * Node file layout (numbers are 32-bit big-endian):
 *   start of list (28 bytes): "OM3DSLC1", size, id (0), reserved (0),
 *                             CRC32, id of the first node
 *   slot node: "OM3DSLT1", size, id, next id, CRC32, state, archive length,
 *              archive CRC32, label (28 bytes), file count, then 32 bytes per
 *              save file name
 * The CRC covers the whole node, computed with the CRC field set to 0.
 *
 * The desktop app can rebuild the catalog from the archives. The menu keeps
 * it up to date when it writes an archive; if updating the catalog fails,
 * the archive itself is still fine.
 */
#ifndef OH_MY_3DO_SAVE_CATALOG_H
#define OH_MY_3DO_SAVE_CATALOG_H
#include "save_names.h"
#include "usb_file.h"

/* The file at the start of the list. */
#define SAVE_CATALOG_ROOT "saves/slot-catalog.3di"

/* The catalog ids of the two recovery archives. */
#define SAVE_CATALOG_RECOVERY  1000000000UL
#define SAVE_CATALOG_RECOVERY2 1000000001UL

/* The largest node: the fixed part plus a name for every possible file. */
#define SAVE_CATALOG_BYTES (68 + SAVE_MAX_FILES * 32)

/* What a slot node can say about its archive. */
#define SAVE_CATALOG_VALID   1 /* a valid archive, summarized here */
#define SAVE_CATALOG_INVALID 2 /* the file is there but is not a valid archive */

/* One node, read or about to be written. The start of the list uses only
 * `id` (always 0) and `next`. */
typedef struct SaveCatalogNode {
    unsigned long id;
    unsigned long next;        /* the next lower id in the list, or 0 */
    unsigned long state;       /* SAVE_CATALOG_VALID or _INVALID */
    unsigned long length;      /* the archive's size */
    unsigned long archive_crc; /* the archive's own header CRC, to notice changes */
    char label[28];            /* the slot's label, up to 24 characters */
    size_t count;              /* save files in the archive */
    char files[SAVE_MAX_FILES][32];
} SaveCatalogNode;

/* One slot in a page of search results. */
typedef struct SaveCatalogMatch {
    unsigned int slot; /* slot index, or SAVE_RECOVERY_SLOT / _RECOVERY2_SLOT */
    char label[25];
    char hint[SAVE_TITLE_BYTES]; /* a game title or file name to show beside it */
    unsigned long state;
} SaveCatalogMatch;

/* A page of search results: set `offset` (how many matches to skip) before
 * searching; the search fills in up to 8 rows and the total match count. */
typedef struct SaveCatalogResults {
    SaveCatalogMatch rows[8];
    unsigned long offset, total;
    size_t count;
} SaveCatalogResults;

/*
 * Reads a node from `length` bytes (`root` set for the start of the list),
 * checking its CRC and contents. `bytes` is briefly changed while checking
 * the CRC and then restored. Returns 0, or -1 if invalid.
 */
int save_catalog_decode(SaveCatalogNode *node, unsigned char *bytes, size_t length, int root);

/* Writes a node into `bytes`. Returns its size, or -1 if the node is invalid
 * or does not fit in `capacity`. */
int save_catalog_encode(const SaveCatalogNode *node, unsigned char *bytes, size_t capacity,
                        int root);

/* The catalog id of a slot index (or recovery slot). */
unsigned long save_catalog_id(unsigned int slot);

/* The slot index for an archive path such as "saves/slot07.3ds". Returns 0,
 * or -1 if the path is not an archive path. */
int save_catalog_slot_from_path(const char *path, unsigned int *slot);

/* Writes the node file's path for catalog id `id` (0 for the start of the
 * list) into `path`, which needs at least 30 bytes. */
void save_catalog_path(char *path, unsigned long id);

/*
 * Records a newly written archive for `slot`, with an optional label: writes
 * its node and links it into the list. Call with the drive's root folder
 * open. Returns 0, or -1 (the archive itself is unaffected).
 */
int save_catalog_update(Ode *ode, OdeReadBuffer reader, unsigned int slot,
                        const SaveArchive *archive, const char *label);

/* Changes a slot's label in the catalog. Returns 0, or -1. */
int save_catalog_relabel(Ode *ode, OdeReadBuffer reader, unsigned int slot, const char *label);

/*
 * Searches every slot for `query` (see save_catalog_matches), filling one
 * page of `out`. Recovery archives are included only with `recovery` set.
 * Returns 0, or -1 if the catalog is missing or damaged.
 */
int save_catalog_search(Ode *ode, OdeReadBuffer reader, const SaveNames *names, const char *query,
                        int recovery, SaveCatalogResults *out);

/*
 * Whether a slot matches a search: every word of `query` (split at spaces)
 * must appear, ignoring case, in the slot's label, its archive's file name,
 * or one of its save files' names or game titles.
 */
int save_catalog_matches(const SaveCatalogNode *node, const SaveNames *names, const char *query);
#endif
