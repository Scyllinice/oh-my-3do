#ifndef OH_MY_3DO_ODE_H
#define OH_MY_3DO_ODE_H

#include "menu_sort.h"

#define ODE_MAX_ENTRIES 2048
#define ODE_MAX_NAME 1024
#define ODE_TOC_BYTES 2048
#define ODE_MAX_DISCS 16 /* FCare PLAYLIST_MAX */

/* Transport returns raw status bytes, including command echo and MEI status.
 * A negative return is a transport error. All commands are seven bytes.
 */
typedef int (*OdeTransfer)(void *context, const unsigned char *command,
                          unsigned char *reply, size_t capacity, size_t *length);
typedef void (*OdeProgress)(void *context, size_t count);

typedef struct Ode {
    OdeTransfer transfer;
    void *context;
    OdeProgress progress;
    void *progress_context;
    char identity[7];
    unsigned char revision[3];
    unsigned char identity_reply[12];
    const char *error;
    unsigned long navigation_key;
    int navigation_known;
    int navigation_failed;
    int transport_uncertain; /* Partial I/O or an unframed reply: no recovery commands. */
    /* While set, media opens from the .ohmy3do folder beside the current
     * folder's discs: no navigation to the root (see UsbArtIndex.beside). */
    int media_beside;
    /* With media_beside: a subfolder of the current folder whose .ohmy3do
     * holds the media (a folder acting as its game), or NULL for this one. */
    const char *media_folder;
    /* Retained across a failed open, when USB diagnostic export cannot run. */
    char file_path[128];
} Ode;

typedef struct OdeListing {
    const MenuEntry **entries;
    size_t count;
    int has_parent;
    unsigned long parent_id;
} OdeListing;

/* Firmware owns resolved file paths after an acknowledged add. These owned
 * display paths remain valid while browsing other directories. Never sort this
 * array: its order is the user's disc-change sequence. */
typedef struct OdePlaylist {
    char *paths[ODE_MAX_DISCS];
    size_t count;
    int synchronized;
    int dirty;
} OdePlaylist;

void ode_listing_init(OdeListing *listing);
void ode_listing_free(OdeListing *listing);
int ode_identify(Ode *ode);
int ode_change_directory(Ode *ode, unsigned long id);
int ode_enter_directory(Ode *ode, const OdeListing *listing,
                        const MenuEntry *entry, int workaround);
int ode_read_directory(Ode *ode, OdeListing *listing);
unsigned long ode_listing_fingerprint(const OdeListing *listing);
/* Read-only, bounded recovery of a known USB root. Never clears a transport
 * fault; output is adopted only after two fresh matching root listings. */
int ode_recover_root(Ode *ode,unsigned long fingerprint,unsigned long volume,OdeListing *out);
/* Read only the target directory record at its existing TOC cursor.
 * 1 = ID/name/type verified, 0 = use full enumeration, -1 = I/O failure. */
int ode_probe_directory(Ode *ode,unsigned long id,const char *name);
int ode_launch(Ode *ode, const MenuEntry *entry);
void ode_playlist_init(OdePlaylist *playlist);
void ode_playlist_dispose(OdePlaylist *playlist);
int ode_playlist_clear(Ode *ode, OdePlaylist *playlist);
int ode_playlist_add(Ode *ode, OdePlaylist *playlist,
                     const MenuEntry *entry, const char *directory);
int ode_playlist_append_local(Ode *ode, OdePlaylist *playlist, const char *path);
int ode_playlist_remove_local(OdePlaylist *playlist, size_t index);
int ode_playlist_move_local(OdePlaylist *playlist, size_t from, size_t to);
int ode_playlist_start(Ode *ode, OdePlaylist *playlist);

#endif
