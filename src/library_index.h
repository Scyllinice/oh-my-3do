/*
 * Reading the library indexes the desktop app writes to .ohmy3do:
 *
 *   library.3di   every disc path on the drive, for Find
 *   metadata.3di  each game's genre, publisher and release date, shown in the
 *                 list and on the details screen
 *
 * Both files share one layout (numbers are 32-bit big-endian):
 *
 *   offset 0   8 bytes  magic: "OM3DLIB1" or "OM3DMET1"
 *          8   4        number of entries
 *         12   4        payload size in bytes
 *         16   4        CRC32 of the payload
 *         20   4        reserved, always 0
 *         24   ...      payload
 *         ...  8 bytes  footer: "END3DL01" or "END3DM01"
 *
 * The payload is a run of strings, each stored as a 2-byte big-endian length
 * (counting its terminating zero) followed by the string and its zero byte.
 * In library.3di each entry is one disc path; in metadata.3di each entry is
 * four strings (path, genre, publisher, release date), sorted by path
 * without regard to case so a game can be found by binary search.
 *
 * Paths are relative to the drive (or volume) root, like
 * "Games/USA/Road Rash (USA)/Road Rash (USA).iso".
 *
 * Why the checksum and footer: the FCare reports reading the full amount
 * asked for even past the end of a file, so a cut-off file would otherwise
 * look complete. The footer must be found exactly where the header says the
 * payload ends, and the CRC catches damage in between.
 */
#ifndef OH3DO_LIBRARY_INDEX_H
#define OH3DO_LIBRARY_INDEX_H
#include "library_navigation.h"
#include "usb_media.h"

/*
 * Loads library.3di into `catalog` (see library_navigation.h), turning each
 * path into a full menu path by putting `prefix` ("/Storage" or
 * "/Storage/VOLUME") in front. Call with the drive's root folder open. Reads
 * only; it never scans folders or writes files.
 *
 * The old catalog is replaced only when the new one loads completely.
 * Returns 0 on success, 1 if `cancel` asked to stop, -1 for an invalid file
 * or a USB failure, or -3 if memory ran out.
 */
int library_index_load(Ode *ode, OdeReadBuffer reader, const char *prefix, LibraryCatalog *catalog,
                       MediaCancel cancel, void *context);

/* Limits on metadata.3di: entries, and bytes of payload plus 16 per entry
 * (the size of the LibraryGameInfo kept for each). */
#define LIBRARY_METADATA_MAX   2048
#define LIBRARY_METADATA_BYTES (192UL * 1024UL)

/* One game's details; every string points into LibraryMetadata.storage. */
typedef struct LibraryGameInfo {
    const char *path;  /* the disc, relative to the drive root */
    const char *genre; /* "" when unknown, as are the next two */
    const char *publisher;
    const char *released; /* release date as text, e.g. "1994-01-01" */
} LibraryGameInfo;

typedef struct LibraryMetadata {
    LibraryGameInfo *entries; /* sorted by path, ignoring case */
    unsigned char *storage;   /* the payload, which the entries point into */
    size_t count;
} LibraryMetadata;

/* Frees the metadata and leaves it empty. */
void library_metadata_free(LibraryMetadata *metadata);

/*
 * Reads a whole metadata.3di file from memory (`size` bytes, header through
 * footer). The old metadata is replaced only on success. Returns 0, -1 for
 * an invalid file, or -3 if memory ran out.
 */
int library_metadata_parse(const unsigned char *data, size_t size, LibraryMetadata *metadata);

/*
 * Loads metadata.3di from the drive (call with the drive's root folder
 * open). The metadata is optional, so a missing or damaged file leaves the
 * old metadata in place. Returns as library_metadata_parse does.
 */
int library_metadata_load(Ode *ode, OdeReadBuffer reader, LibraryMetadata *metadata);

/*
 * Finds a game's details by its disc path, ignoring case. Searches only what
 * is already loaded, so it never touches the drive. NULL if not found.
 */
const LibraryGameInfo *library_metadata_find(const LibraryMetadata *metadata, const char *path);

/*
 * Whether `path` is a relative path to a disc image the menu may open: it
 * ends in .iso or .cue, every folder name is safe (not empty, "." or "..", no
 * control characters, backslashes or colons) and it is not nested deeper
 * than LIBRARY_DEPTH folders.
 */
int library_path_safe(const char *path);
#endif
