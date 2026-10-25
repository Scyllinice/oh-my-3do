/*
 * Talking to the optical drive emulator (ODE): browsing folders and
 * launching discs.
 *
 * The menu runs on a 3DO whose CD drive has been replaced by an FCare ODE,
 * which serves disc images from a USB drive. The console talks to it through
 * the CD drive's command channel: the menu sends 7-byte commands and gets
 * short status replies (the first byte echoes the command; the last carries
 * status bits). The FCare adds its own commands for browsing and launching:
 *
 *   0x93  identify: the reply names the firmware ("LOCODE") and its version
 *   0xC0  change folder: enter the folder with a given id (or select a file)
 *   0xC1  read 16 bytes of the current folder's listing
 *   0xC3  clear the disc queue
 *   0xC4  add a disc to the queue
 *   0xC5  start the queue (the console then boots the first disc)
 * and file commands, used by usb_file.h.
 *
 * Ids. Every entry in a folder has a 32-bit id. The top byte says which USB
 * volume it is on; the low 24 bits number the entry within its folder. An id
 * whose low 24 bits are all ones means "the parent folder", and on drives with
 * several volumes the top-level list shows the volumes themselves, whose ids
 * have zero low bits.
 *
 * Folder listings. The FCare keeps a 2048-byte listing of part of the current
 * folder, read 16 bytes at a time with 0xC1. It is a run of records:
 *     flags (4 bytes: 1 = file, 2 = folder, 0xFFFFFFFF = end of listing)
 *     id (4 bytes)
 *     name length (4 bytes, counting the terminating zero)
 *     the name and its zero
 * all numbers big-endian. Big folders need several listings: each one starts
 * after a given entry number.
 *
 * Transport. The actual byte exchange is supplied by the caller (OdeTransfer),
 * so tests can stand in for the console's hardware.
 */
#ifndef OH_MY_3DO_ODE_H
#define OH_MY_3DO_ODE_H

#include "menu_sort.h"

/* Limits: entries in one folder, the longest name (with its zero), the size
 * of the FCare's listing and data buffer, and the FCare's disc queue length. */
#define ODE_MAX_ENTRIES 2048
#define ODE_MAX_NAME    1024
#define ODE_TOC_BYTES   2048
#define ODE_MAX_DISCS   16

/*
 * Sends one 7-byte command and receives its status reply (up to `capacity`
 * bytes, length in *length). Returns a negative number if the exchange
 * itself failed.
 */
typedef int (*OdeTransfer)(void *context, const unsigned char *command, unsigned char *reply,
                           size_t capacity, size_t *length);

/* Called while a long folder listing is read, with the entries so far. */
typedef void (*OdeProgress)(void *context, size_t count);

/* The connection to the ODE, and what the menu knows about its state. */
typedef struct Ode {
    OdeTransfer transfer;
    void *context;
    OdeProgress progress; /* optional; NULL for none */
    void *progress_context;

    char identity[7];          /* e.g. "LOCODE" for the FCare */
    unsigned char revision[3]; /* firmware version numbers */
    unsigned char identity_reply[12];
    const char *error; /* the last failure, for the screen; NULL if none */

    /*
     * The FCare's folder-change bug (see MENU_WORKAROUND_AUTO): it ignores a
     * change to the same entry number it changed to last. navigation_key is
     * that last entry number (low 24 bits), valid when navigation_known.
     */
    unsigned long navigation_key;
    int navigation_known;

    /*
     * Set when the menu can no longer be sure which folder the ODE is in
     * (see art_navigation.h). All browsing stops until a restart.
     */
    int navigation_failed;

    /*
     * Set when an exchange was cut off or a reply did not look like one:
     * the command channel may be out of step, so even recovery commands are
     * not sent.
     */
    int transport_uncertain;

    /* While set, media files open from the .ohmy3do folder beside the
     * current folder's discs, not from the drive root (see UsbArtIndex.beside). */
    int media_beside;
    /* With media_beside: a subfolder of the current folder whose .ohmy3do
     * holds the media (a folder acting as its game), or NULL for this one. */
    const char *media_folder;

    /* The last file path opened, kept for diagnostics after a failure. */
    char file_path[128];
} Ode;

/* One folder's entries, sorted for display (see menu_sort.h). */
typedef struct OdeListing {
    const MenuEntry **entries;
    size_t count;
    int has_parent;          /* the folder has a ".." entry (it is not the top) */
    unsigned long parent_id; /* that entry's id */
} OdeListing;

/*
 * The disc queue: discs to boot one after another (a multi-disc game). The
 * FCare keeps its own copy, resolved when each disc is added; this list
 * holds their paths for display and editing, in queue order (never sorted).
 */
typedef struct OdePlaylist {
    char *paths[ODE_MAX_DISCS];
    size_t count;
    /* The FCare's queue is known to match this list. Cleared whenever a
     * command's outcome is unknown; only a confirmed clear sets it again. */
    int synchronized;
    /* Edited here (reordered, discs removed) but not yet sent to the FCare. */
    int dirty;
} OdePlaylist;

void ode_listing_init(OdeListing *listing);
void ode_listing_free(OdeListing *listing);

/* Asks the ODE who it is (fills identity and revision). Returns 0, or -1. */
int ode_identify(Ode *ode);

/*
 * Sends the change-folder command for `id`. On failure sets
 * navigation_failed (the current folder is then unknown) and returns -1.
 */
int ode_change_directory(Ode *ode, unsigned long id);

/*
 * Enters the folder `entry` of `listing` (the current folder). With
 * `workaround` set, avoids the FCare's folder-change bug by first changing
 * to another entry when needed. Returns 0, or -1.
 */
int ode_enter_directory(Ode *ode, const OdeListing *listing, const MenuEntry *entry,
                        int workaround);

/* Reads the whole current folder into `listing`, sorted. Returns 0, or -1. */
int ode_read_directory(Ode *ode, OdeListing *listing);

/* A hash of a listing's ids, kinds and names, to recognize a folder again. */
unsigned long ode_listing_fingerprint(const OdeListing *listing);

/*
 * Tries to get back to the drive's top folder after browsing failed, without
 * writing anything. Refuses if the transport is in doubt. The result is only
 * accepted when two fresh listings both match `fingerprint` (the top folder
 * as it was at startup; `volume` is the volume id, or 0). Returns 0 with the
 * listing in `out`, or -1.
 */
int ode_recover_root(Ode *ode, unsigned long fingerprint, unsigned long volume, OdeListing *out);

/*
 * Checks quickly that the folder `id` called `name` is still there, by
 * reading only its own listing record. Returns 1 if it is, 0 if the full
 * listing must be used instead, or -1 on a USB failure.
 */
int ode_probe_directory(Ode *ode, unsigned long id, const char *name);

/* Boots one disc: replaces the queue with it and starts it. Returns 0, or -1. */
int ode_launch(Ode *ode, const MenuEntry *entry);

void ode_playlist_init(OdePlaylist *playlist);
/* Frees the list's paths (does not touch the FCare's queue). */
void ode_playlist_dispose(OdePlaylist *playlist);
/* Clears the FCare's queue and this list. Returns 0, or -1. */
int ode_playlist_clear(Ode *ode, OdePlaylist *playlist);
/* Adds a disc of the current folder (whose full path is `directory`) to the
 * FCare's queue and this list. Returns 0, or -1. */
int ode_playlist_add(Ode *ode, OdePlaylist *playlist, const MenuEntry *entry,
                     const char *directory);
/* Edits this list only (marking it dirty): append a path, remove or move one. */
int ode_playlist_append_local(Ode *ode, OdePlaylist *playlist, const char *path);
int ode_playlist_remove_local(OdePlaylist *playlist, size_t index);
int ode_playlist_move_local(OdePlaylist *playlist, size_t from, size_t to);
/* Starts the FCare's queue. Returns 0, or -1. */
int ode_playlist_start(Ode *ode, OdePlaylist *playlist);

#endif
