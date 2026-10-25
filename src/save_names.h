/*
 * Save names and slot labels.
 *
 * Games store their saves as small files in the console's NVRAM, with names
 * like "RRASH.SAV". This module turns those file names into game titles for
 * the save manager, and learns new ones by watching which files change while
 * a game runs. It also reads and writes the labels people give their backup
 * slots.
 */
#ifndef OH_MY_3DO_SAVE_NAMES_H
#define OH_MY_3DO_SAVE_NAMES_H
#include "save_archive.h"

/*
 * The largest save-names.txt or saves-learned.txt the menu reads.
 *
 * save-names.txt is written by the PC tools from the discs; saves-learned.txt
 * by the menu itself. Each line is "=FILE<TAB>Title" (one exact file name) or
 * "*PREFIX<TAB>Title" (every file name starting with PREFIX).
 */
#define SAVE_NAMES_BYTES 32768

/* Room for a save snapshot file (see save_snapshot_format). */
#define SAVE_SNAPSHOT_BYTES 8192

/* The longest game title in a snapshot, including its terminating zero. */
#define SAVE_TITLE_BYTES 64

/* One name: a file name (or prefix) and the game title it maps to. */
typedef struct SaveNameEntry {
    const char *key;   /* file name, or the start of file names when prefix is set */
    const char *title; /* game title shown for matching files */
    int prefix;        /* 1: key matches any file name that starts with it */
    int learned;       /* 1: came from saves-learned.txt, which always wins */
} SaveNameEntry;

/* All names loaded from the drive. Free with save_names_free. */
typedef struct SaveNames {
    char *storage;          /* both files' text, which the entries point into */
    SaveNameEntry *entries; /* one per valid line */
    size_t count;
} SaveNames;

/*
 * Loads names from save-names.txt (`disc`) and saves-learned.txt
 * (`learned`). Neither text needs a terminating zero; both are copied, so the
 * caller may free them afterwards. Returns 0, or -1 if a text is too long or
 * memory runs out (names is then empty).
 */
int save_names_parse(SaveNames *names, const char *disc, size_t disc_length, const char *learned,
                     size_t learned_length);

/* Frees everything save_names_parse allocated and empties `names`. */
void save_names_free(SaveNames *names);

/*
 * The game title for NVRAM file `file`, or NULL if none is known.
 * Learned names win, then exact names from the discs, then the longest
 * matching prefix; the built-in list fills in anything still unnamed.
 */
const char *save_names_find(const SaveNames *names, const char *file);

/*
 * Writes a snapshot of NVRAM just before a game starts:
 *
 *     OH3DO_SAVESNAP1
 *     Game title
 *     FILE<TAB>size<TAB>crc32
 *     ...
 *
 * with one line per NVRAM file (size in decimal, CRC as eight hex digits).
 * At the next startup, save_snapshot_learn compares NVRAM against it.
 * Returns 0, or -1 if the title is empty, too long or contains control
 * characters, or `capacity` might be too small.
 */
int save_snapshot_format(char *out, size_t capacity, const char *title, size_t title_length,
                         const SaveArchive *nvram);

/*
 * Learns names from a snapshot: every NVRAM file that is new or changed since
 * the snapshot, and has no name yet, must belong to the game that was
 * launched. For each one, appends "=FILE<TAB>Title\n" to the zero-terminated
 * text in `learned` (which holds `capacity` bytes). Stops early when it is
 * full. Returns how many names were added, or -1 for a damaged snapshot.
 */
int save_snapshot_learn(const char *snapshot, size_t length, const SaveArchive *nvram,
                        const SaveNames *names, char *learned, size_t capacity);

/*
 * Backup slot labels. Each file saves/slot-labels-N.txt labels the eight
 * slots on page N of the backup list, one "S<TAB>Label" line per labelled
 * slot, where S is the slot's position on the page from 1 to 8.
 */
#define SAVE_SLOTS_PER_LABEL_FILE  8
#define SAVE_SLOT_LABEL_BYTES      25  /* longest label plus its terminating zero */
#define SAVE_SLOT_LABEL_FILE_BYTES 512 /* largest labels file the menu reads */

typedef struct SaveSlotLabels {
    /* An empty string means the slot has no label. */
    char labels[SAVE_SLOTS_PER_LABEL_FILE][SAVE_SLOT_LABEL_BYTES];
} SaveSlotLabels;

/*
 * Reads a labels file. Reading stops at a zero byte, so a shorter file
 * written over a longer one is read correctly. Returns 0, or -1 for a damaged
 * file, leaving `out` unchanged.
 */
int save_slot_labels_parse(SaveSlotLabels *out, const char *text, size_t length);

/* Writes a labels file as zero-terminated text. Returns 0, or -1 if it does
 * not fit or a label is invalid. */
int save_slot_labels_format(const SaveSlotLabels *labels, char *out, size_t capacity);
#endif
