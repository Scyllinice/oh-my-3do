/*
 * Game descriptions.
 *
 * The desktop app writes one small text file per described game, which the
 * menu reads only when someone opens that game's details (hold C, tap P).
 * Each file looks like:
 *
 *     OM3DINFO1
 *     Games/USA/Road Rash (USA)/Road Rash (USA).iso
 *     This is a remake of the original Road Rash...
 *     1a2b3c4d
 *
 * The second line is the disc's path, so a file can never be shown for the
 * wrong game, and the last line is a CRC32 checksum of the two lines before
 * it, so a damaged file is ignored rather than shown half-garbled.
 */
#ifndef OH3DO_GAME_INFO_H
#define OH3DO_GAME_INFO_H
#include <stddef.h>

/* The longest description the menu shows, plus its terminating zero. */
#define GAME_DESCRIPTION_BYTES 2049

/*
 * Writes the description file's name for the disc at `path` (relative to the
 * drive root) into `name`, which needs at least 18 bytes: "info/" followed by
 * eight hex digits of the path's CRC32 and ".txt". On a drive the desktop app
 * prepared, the file sits beside the disc, so callers use the part after
 * "info/"; drives laid out by hand keep it under .ohmy3do/info/ at the root.
 */
void game_info_filename(const char *path, char *name);

/*
 * Checks a description file's `length` bytes and copies its description into
 * `description` (`capacity` bytes) as a zero-terminated string. The file must
 * be for the disc at `path` (compared without regard to case). Returns 0, or
 * -1 if the file is damaged, belongs to another disc or does not fit; the
 * description is then empty.
 */
int game_info_parse(const char *bytes, size_t length, const char *path, char *description,
                    size_t capacity);
#endif
