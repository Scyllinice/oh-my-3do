#ifndef OH_MY_3DO_SAVE_CATALOG_H
#define OH_MY_3DO_SAVE_CATALOG_H
#include "save_names.h"
#include "usb_file.h"
#define SAVE_CATALOG_ROOT      "saves/slot-catalog.3di"
#define SAVE_CATALOG_RECOVERY  1000000000UL
#define SAVE_CATALOG_RECOVERY2 1000000001UL
#define SAVE_CATALOG_BYTES     (68 + SAVE_MAX_FILES * 32)
typedef struct SaveCatalogNode {
    unsigned long id, next, state, length, archive_crc;
    char label[28];
    size_t count;
    char files[SAVE_MAX_FILES][32];
} SaveCatalogNode;
typedef struct SaveCatalogMatch {
    unsigned int slot;
    char label[25], hint[SAVE_TITLE_BYTES];
    unsigned long state;
} SaveCatalogMatch;
typedef struct SaveCatalogResults {
    SaveCatalogMatch rows[8];
    unsigned long offset, total;
    size_t count;
} SaveCatalogResults;
int save_catalog_decode(SaveCatalogNode *node, unsigned char *bytes, size_t length, int root);
int save_catalog_encode(const SaveCatalogNode *node, unsigned char *bytes, size_t capacity,
                        int root);
unsigned long save_catalog_id(unsigned int slot);
int save_catalog_slot_from_path(const char *path, unsigned int *slot);
void save_catalog_path(char *path, unsigned long id);
/* Root-directory operations. Missing/invalid catalogs require desktop refresh;
 * failures never invalidate an archive that has already passed verification. */
int save_catalog_update(Ode *ode, OdeReadBuffer reader, unsigned int slot,
                        const SaveArchive *archive, const char *label);
int save_catalog_relabel(Ode *ode, OdeReadBuffer reader, unsigned int slot, const char *label);
int save_catalog_search(Ode *ode, OdeReadBuffer reader, const SaveNames *names, const char *query,
                        int recovery, SaveCatalogResults *out);
int save_catalog_matches(const SaveCatalogNode *node, const SaveNames *names, const char *query);
#endif
