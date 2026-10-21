/* Host fixture for the actual FCare setTocLevel function inserted below.
 * Only filesystem/device dependencies are stubbed; navigation logic is taken
 * directly from the checked-out firmware, before and after applying our patch.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ode.h"
#include "art_navigation.h"
#include "library_navigation.h"
#include "menu_ui.h"

typedef int FRESULT;
typedef struct { int offset; } DIR;
typedef struct { DIR dir; } dir_t;
typedef struct { char fname[128]; unsigned int fattrib; } FILINFO;
#define AM_DIR 0x10
#define LOG_SATA(...) ((void)0)
#define rindex strrchr /* BSD spelling used by the embedded firmware */

static struct {
    unsigned char dev_addr;
    dir_t *curDir;
    char *curPath;
} currentImage;
static int current_toc;
static int current_toc_level;
static int current_toc_offset;
static int device_count = 1;
static int hidden_files;
static int navigation_helper;
static int calls, toc_calls;
static int changed_child;
static int recovery_reject_parent,recovery_stuck_parent,recovery_bad_echo,recovery_io_failure;
static int recovery_other_identity,recovery_bad_read,commands;
static unsigned char toc_cache[2048];

static int getTocEntry(int index) { return index & 0xffffff; }
static int getTocLevel(void) { return current_toc_level; }
static int getDeviceCount(void) { return device_count; }
static bool isUp(int index) { return getTocEntry(index) == 0xffffff; }
static dir_t *getNewDir(void) { return (dir_t *)calloc(1, sizeof(dir_t)); }
static FRESULT f_closedir(DIR *dir) { (void)dir; return 0; }
static FRESULT f_opendir(DIR *dir, const char *path)
{
    (void)path;
    dir->offset = 0;
    return 0;
}

static bool getNextValidToc(FILINFO *info)
{
    int index = currentImage.curDir->dir.offset++;
    memset(info, 0, sizeof(*info));
    if (!strcmp(currentImage.curPath, "0:") || !strcmp(currentImage.curPath, "0://")) {
        if (index < 2) {
            strcpy(info->fname, index == 0 ? "Alpha" : "Beta");
            info->fattrib = AM_DIR;
        } else if (index == 2 && !hidden_files) strcpy(info->fname, "boot.iso");
    } else if (!strcmp(currentImage.curPath, "0:\\Alpha")) {
        if (index == 0) {
            strcpy(info->fname, "Child");
            info->fattrib = AM_DIR;
        } else if (index == 1 && !hidden_files) strcpy(info->fname, navigation_helper ? "Oh My 3DO - Folder Navigation Helper (Keep).iso" : "Disc 1.iso");
    } else if (!strcmp(currentImage.curPath, "0:\\Alpha\\Child") && changed_child && !index) {
        strcpy(info->fname, "Unexpected.iso");
    }
    return info->fname[0] != 0;
}

/* INSERT_FIRMWARE_SET_TOC_LEVEL */

static void put32(unsigned char *out, unsigned long value)
{
    out[0]=(unsigned char)(value>>24); out[1]=(unsigned char)(value>>16);
    out[2]=(unsigned char)(value>>8); out[3]=(unsigned char)value;
}

static size_t toc_append(size_t pos, unsigned long type, unsigned long id, const char *name)
{
    size_t length=strlen(name)+1;
    put32(toc_cache+pos,type); put32(toc_cache+pos+4,id);
    put32(toc_cache+pos+8,(unsigned long)length);
    memcpy(toc_cache+pos+12,name,length);
    return pos+12+length;
}

static void populate_toc(unsigned long cursor)
{
    size_t pos=0;
    unsigned long ordinal=0;
    FILINFO info;
    currentImage.curDir->dir.offset=0;
    memset(toc_cache,255,sizeof(toc_cache));
    if (!currentImage.curPath) {
        toc_append(toc_append(0,2,0x01000000UL,"USB 0"),2,0x02000000UL,"USB 1");return;
    }
    if (!cursor && (strchr(currentImage.curPath,'\\') || (device_count>1 && current_toc_level>0)))
        pos=toc_append(pos,2,0x01ffffffUL,"..");
    while (getNextValidToc(&info)) {
        ++ordinal;
        if (ordinal > cursor)
            pos=toc_append(pos,(info.fattrib & AM_DIR) ? 2 : 1,0x01000000UL|ordinal,info.fname);
    }
}

static int transfer(void *context, const unsigned char *packet,
                    unsigned char *reply, size_t capacity, size_t *length)
{
    unsigned long id = ((unsigned long)packet[1] << 24) |
        ((unsigned long)packet[2] << 16) | ((unsigned long)packet[3] << 8) | packet[4];
    (void)context;
    ++commands;
    if (packet[0]==0x93) {
        memset(reply,0,capacity);reply[0]=0x93;memcpy(reply+2,"LOCODE",6);*length=12;
        if (recovery_io_failure) return -1;
        if (recovery_bad_echo) reply[0]=0;
        if (recovery_other_identity) memcpy(reply+2,"OTHER!",6);
        return 0;
    }
    if (packet[0] == 0xc1) {
        if (recovery_bad_read) return -1;
        ++toc_calls;
        unsigned int offset=(unsigned int)packet[5]*256+packet[6];
        if (!offset) populate_toc(id);
        if (offset > 2032 || capacity < 18) abort();
        reply[0]=0xc1;
        memcpy(reply+1,toc_cache+offset,16);
        reply[17]=0;
        *length=18;
        return 0;
    }
    if (packet[0] != 0xc0 || capacity < 2) abort();
    ++calls;
    if ((id&0xffffffUL)==0xffffffUL && recovery_reject_parent) {
        reply[0]=0xc0;reply[1]=0x10;*length=2;return 0;
    }
    if ((id&0xffffffUL)==0xffffffUL && recovery_stuck_parent) {
        reply[0]=0xc0;reply[1]=0;*length=2;return 0;
    }
    setTocLevel((int)id);
    reply[0] = 0xc0;
    reply[1] = 0;
    *length = 2;
    return 0;
}

static void reset_fixture(int key, int depth)
{
    free(currentImage.curPath);
    free(currentImage.curDir);
    currentImage.curDir = getNewDir();
    currentImage.curPath = (char *)malloc(3);
    strcpy(currentImage.curPath, "0:");
    currentImage.dev_addr = 1;
    current_toc = key;
    current_toc_level = depth;
    current_toc_offset = 0;
    device_count = 1;
    hidden_files = calls = 0;
    navigation_helper = 0;
    changed_child = 0;
    recovery_reject_parent=recovery_stuck_parent=recovery_bad_echo=recovery_io_failure=0;
    recovery_other_identity=recovery_bad_read=commands=0;
}

static int root_read_mode;
static int root_read(void *context)
{
    (void)context;
    if (strcmp(currentImage.curPath,"0:")) abort();
    if (root_read_mode == 2) changed_child=1;
    return root_read_mode == 1 ? -1 : 0;
}

static int file_action_calls;
static int child_read_mode,child_read_calls;
static int child_read(void *context)
{
    Ode *ode=(Ode *)context;
    if (strcmp(currentImage.curPath,"0:\\Alpha\\Child") || ode->media_folder) abort();
    child_read_calls=calls;
    if (child_read_mode==2) hidden_files=1; /* Parent no longer matches. */
    if (child_read_mode==3) ode->navigation_failed=1; /* Transport timeout. */
    return child_read_mode==1 ? -1 : 0;
}
static int file_action(void *context, const MenuEntry *entry, const char *directory)
{
    (void)context;
    if (strcmp(currentImage.curPath,"0:\\Alpha") || strcmp(entry->name,"Disc 1.iso") || strcmp(directory,"/Storage/Alpha")) abort();
    ++file_action_calls; return 0;
}

static int check(int ok, const char *name)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    return ok ? 0 : 1;
}

int main(void)
{
    int failed = 0;
    Ode ode;
    const MenuEntry alpha = {0x01000001UL, 1, "Alpha"};
    const MenuEntry beta = {0x01000002UL, 1, "Beta"};
    const MenuEntry boot = {0x01000003UL, 0, "boot.iso"};
    const MenuEntry child = {0x01000001UL, 1, "Child"};
    const MenuEntry disc = {0x01000002UL, 0, "Disc 1.iso"};
    const MenuEntry helper = {0x01000002UL, 0, "Oh My 3DO - Folder Navigation Helper (Keep).iso"};
    const MenuEntry *root_entries[] = {&alpha, &beta, &boot};
    const MenuEntry *child_entries[] = {&child, &disc};
    OdeListing root = {root_entries, 3, 0, 0};
    OdeListing nested = {child_entries, 2, 1, 0x01ffffffUL};
    OdeListing snapshot;
    ArtRouteStep route[2]={{0x01000001UL,"Alpha"},{0x01000001UL,"Child"}};
    /* A stale key must not block the first real folder at the root. */
    reset_fixture(0x01000001, 0);
    setTocLevel(0x01000001);
    failed += check(!strcmp(currentImage.curPath, "0:\\Alpha") &&
                    current_toc_level == 1, "stale-root-ID");

    /* Alpha's local ID is 1; its first child also has local ID 1. */
    reset_fixture(0x01000000, 0);
    setTocLevel(0x01000001);
    setTocLevel(0x01000001);
    failed += check(!strcmp(currentImage.curPath, "0:\\Alpha\\Child") &&
                    current_toc_level == 2, "repeated-child-ID");

    reset_fixture(0x01000000, 0);
    setTocLevel(0x01000002);
    setTocLevel(0x01ffffff);
    setTocLevel(0x01000001);
    failed += check(!strcmp(currentImage.curPath, "0:\\Alpha") &&
                    current_toc_level == 1, "sibling-back-first-folder");
    setTocLevel(0x01ffffff);
    setTocLevel(0x01000001);
    failed += check(!strcmp(currentImage.curPath, "0:\\Alpha") &&
                    current_toc_level == 1, "back-and-reenter");

    /* A repeated zero-entry device selection must retain its old semantics. */
    reset_fixture(0x01000000, 1);
    setTocLevel(0x01000000);
    failed += check(!strcmp(currentImage.curPath, "0:") &&
                    current_toc_level == 1, "repeated-device-selection");

    reset_fixture(0, 0);
    setTocLevel(0);
    failed += check(current_toc == 0x01000000 && current_toc_level == 0,
                    "single-device-root-initialization");

    memset(&ode, 0, sizeof(ode));
    ode.transfer = transfer;
    reset_fixture(0x01000001, 0);
    failed += check(ode_enter_directory(&ode, &root, &alpha, 1) == 0 &&
                    !strcmp(currentImage.curPath, "0:\\Alpha") && calls == 2,
                    "menu-workaround-unknown-startup-key");
    failed += check(ode_enter_directory(&ode, &nested, &child, 1) == 0 &&
                    !strcmp(currentImage.curPath, "0:\\Alpha\\Child"),
                    "menu-workaround-repeated-child-ID");

    reset_fixture(0x01000001, 1);
    hidden_files = 1;
    root.count = 2;
    ode.navigation_known = 1;
    ode.navigation_key = 1;
    failed += check(ode_enter_directory(&ode, &root, &alpha, 1) == 0 &&
                    !strcmp(currentImage.curPath, "0:\\Alpha") && calls == 3,
                    "menu-workaround-sibling-detour");
    nested.count = 1;
    calls = 0;
    failed += check(ode_enter_directory(&ode, &nested, &child, 1) < 0 &&
                    !strcmp(currentImage.curPath, "0:\\Alpha") && calls == 0,
                    "menu-workaround-no-safe-alternative");

    /* The same folder-only chain becomes navigable with a hidden UI pivot.
     * transfer() accepts only C0/C1 here: no queue/mount command is allowed. */
    hidden_files = 0; navigation_helper = 1;
    nested.count = 2; child_entries[1] = &helper; calls = 0;
    failed += check(menu_support_entry(helper.name,0,0) &&
                    ode_enter_directory(&ode, &nested, &child, 1) == 0 &&
                    !strcmp(currentImage.curPath, "0:\\Alpha\\Child") && calls == 2,
                    "menu-workaround-folder-only-chain-with-helper");
    child_entries[1] = &disc;

    reset_fixture(0x01000000, 0);
    ode.navigation_known = 1;
    ode.navigation_key = 0;
    failed += check(ode_enter_directory(&ode, &root, &alpha, 1) == 0 && calls == 1,
                    "menu-workaround-no-collision-single-command");
    reset_fixture(0x01000000, 0);
    failed += check(ode_enter_directory(&ode, &root, &alpha, 0) == 0 && calls == 1,
                    "menu-workaround-disabled-single-command");

    reset_fixture(0x01000000,0);
    ode.navigation_failed=0;
    ode.navigation_known=1;
    ode.navigation_key=0;
    root.count=3;
    nested.count=2;
    {
        int mode;
        for (mode=0;mode<4;++mode) {
            reset_fixture(0x01000000,0);
            ode.navigation_failed=0; ode.navigation_known=1; ode.navigation_key=0;
            if (ode_enter_directory(&ode,&root,&alpha,1)<0) abort();
            ode_listing_init(&snapshot);
            if (ode_read_directory(&ode,&snapshot)<0) abort();
            child_read_mode=mode; ode.media_beside=1; ode.media_folder="Child";
            failed += check(art_in_folder(&ode,&snapshot,&child,1,child_read,&ode)==
                            (mode<2 ? (mode==1 ? -1 : 0) : -2),
                            "short-media-path-result");
            failed += check(ode.media_folder && !strcmp(ode.media_folder,"Child"),"short-media-path-context-restored");
            if (mode<2) failed += check(!strcmp(currentImage.curPath,"0:\\Alpha") &&
                                       !ode.navigation_failed,"short-media-path-parent-restored");
            else failed += check(ode.navigation_failed,"short-media-path-unsafe-return-blocked");
            if (mode==3) failed += check(calls==child_read_calls,"short-media-path-no-navigation-after-timeout");
            ode_listing_free(&snapshot);
        }
        ode.media_beside=0; ode.media_folder=NULL;
        reset_fixture(0x01000000,0);
        ode.navigation_failed=0; ode.navigation_known=1; ode.navigation_key=0;
    }
    ode_listing_init(&snapshot);
    if (ode_enter_directory(&ode,&root,&alpha,1) < 0 ||
        ode_enter_directory(&ode,&nested,&child,1) < 0 ||
        ode_read_directory(&ode,&snapshot) < 0) abort();
    failed += check(art_at_root(&ode,route,2,&snapshot,1,root_read,NULL) == 0 &&
                    !strcmp(currentImage.curPath,"0:\\Alpha\\Child") && !ode.navigation_failed,
                    "USB-cover-root-roundtrip-repeated-IDs");
    {
        int slow, fast;
        root_read_mode=0; toc_calls=0;
        failed += check(!art_at_root(&ode,route,2,&snapshot,1,root_read,NULL),"legacy-art-restore"); slow=toc_calls;
        strcpy(ode.identity,"LOCODE"); toc_calls=0;
        failed += check(!art_at_root(&ode,route,2,&snapshot,1,root_read,NULL) &&
                        !strcmp(currentImage.curPath,"0:\\Alpha\\Child") && !ode.navigation_failed,
                        "fast-art-restore-keeps-folder-workaround"); fast=toc_calls;
        failed += check(fast<slow,"fast-art-restore-reduces-TOC-reads");
    }
    root_read_mode=1;
    failed += check(art_at_root(&ode,route,2,&snapshot,1,root_read,NULL) == -1 &&
                    !strcmp(currentImage.curPath,"0:\\Alpha\\Child") && !ode.navigation_failed,
                    "USB-missing-cover-restores-folder");
    root_read_mode=2;
    failed += check(art_at_root(&ode,route,2,&snapshot,1,root_read,NULL) == -2 &&
                    ode.navigation_failed, "USB-cover-changed-folder-disables-navigation");
    ode_listing_free(&snapshot);
    /* Exercise recovery against the actual, unmodified setTocLevel, including
     * a rejected parent command leaving the firmware inside the game folder. */
    {
        OdeListing recovered,known;
        unsigned long fingerprint;
        int mode;
        ode_listing_init(&recovered);ode_listing_init(&known);
        for (mode=0;mode<9;++mode) {
            int before,before_commands;
            reset_fixture(0x01000000,0);memset(&ode,0,sizeof(ode));
            ode.transfer=transfer;strcpy(ode.identity,"LOCODE");
            if (ode_read_directory(&ode,&known)<0) abort();
            fingerprint=ode_listing_fingerprint(&known);ode_listing_free(&known);
            if (ode_enter_directory(&ode,&root,&alpha,1)<0 || ode_enter_directory(&ode,&nested,&child,1)<0) abort();
            recovery_reject_parent=1;
            if (ode_change_directory(&ode,0x01ffffffUL)>=0 || !ode.navigation_failed || strcmp(currentImage.curPath,"0:\\Alpha\\Child")) abort();
            recovery_reject_parent=0;
            if (mode==1) ode.transport_uncertain=1;
            if (mode==2) recovery_bad_echo=1;
            if (mode==3) recovery_reject_parent=1;
            if (mode==4) recovery_stuck_parent=1;
            if (mode==5) ++fingerprint;
            if (mode==6) recovery_io_failure=1;
            if (mode==7) recovery_other_identity=1;
            if (mode==8) recovery_bad_read=1;
            before=calls;before_commands=commands;
            {
                int result=ode_recover_root(&ode,fingerprint,0,&recovered);
                if (!mode) failed+=check(!result && !ode.navigation_failed && recovered.count==3 &&
                    !strcmp(currentImage.curPath,"0:"),"recovery-verified-root-after-rejected-parent");
                else failed+=check(result<0 && ode.navigation_failed && !recovered.entries &&
                    (mode!=1 || commands==before_commands) && (mode!=4 || calls-before==32),"recovery-refuses-unsafe-or-unverified-root");
                if (ode.transport_uncertain) {
                    before_commands=commands;
                    if (!ode_recover_root(&ode,fingerprint,0,&recovered) || commands!=before_commands) abort();
                }
            }
            ode_listing_free(&recovered);
        }
        reset_fixture(0x01000000,1);device_count=2;memset(&ode,0,sizeof(ode));
        ode.transfer=transfer;strcpy(ode.identity,"LOCODE");
        if (ode_read_directory(&ode,&known)<0) abort();
        fingerprint=ode_listing_fingerprint(&known);ode_listing_free(&known);
        if (ode_enter_directory(&ode,&root,&alpha,1)<0) abort();
        ode.navigation_failed=1;
        failed+=check(!ode_recover_root(&ode,fingerprint,0x01000000UL,&recovered) &&
            !strcmp(currentImage.curPath,"0://") && recovered.has_parent && current_toc_level==1,
            "recovery-restores-selected-volume-with-multiple-devices");
        ode_listing_free(&recovered);
    }
    reset_fixture(0x01000000,1);
    ode.navigation_failed=0;
    root_read_mode=0;
    route[0].id=0x01000000UL;
    route[0].name="USB 0";
    failed += check(art_at_root(&ode,route,1,&root,1,root_read,NULL) == 0 && calls == 0,
                    "USB-cover-device-root-stays-on-volume");
    reset_fixture(0x01000000,0);
    ode.navigation_failed=0; ode.navigation_known=1; ode.navigation_key=0;
    root.count=3; nested.count=2;
    route[0].id=alpha.toc_id; route[0].name=alpha.name;
    route[1].id=child.toc_id; route[1].name=child.name;
    if (ode_enter_directory(&ode,&root,&alpha,1) < 0 || ode_enter_directory(&ode,&nested,&child,1) < 0 || ode_read_directory(&ode,&snapshot) < 0) abort();
    failed += check(!library_visit(&ode,route,2,&snapshot,1,"/Storage/Alpha/Disc 1.iso",file_action,NULL) &&
                    file_action_calls == 1 && !strcmp(currentImage.curPath,"0:\\Alpha\\Child") && !ode.navigation_failed,
                    "queue-rebuild-visits-file-and-restores-repeated-IDs");
    failed += check(library_visit(&ode,route,2,&snapshot,1,"/Storage/Beta/Missing.iso",file_action,NULL) == -1 &&
                    !strcmp(currentImage.curPath,"0:\\Alpha\\Child") && !ode.navigation_failed,
                    "missing-queue-path-restores-browser");
    {
        LibraryCatalog catalog;
        memset(&catalog,0,sizeof(catalog));
        failed += check(!library_catalog_build(&ode,route,2,&snapshot,1,&catalog,NULL,NULL) &&
                        catalog.count==1 && !strcmp(catalog.paths[0],"/Storage/Alpha/Disc 1.iso") &&
                        !strcmp(currentImage.curPath,"0:\\Alpha\\Child") && !ode.navigation_failed,
                        "library-search-restores-repeated-IDs");
        library_catalog_free(&catalog);
    }
    {
        LibraryLocation *location=(LibraryLocation *)malloc(sizeof(*location));
        if (!location) abort();
        failed += check(!library_open_directory(&ode,route,2,&snapshot,1,"/Storage/Alpha/Child",location) &&
                        location->depth == 2 && !strcmp(currentImage.curPath,"0:\\Alpha\\Child"),
                        "remembered-folder-restores-repeated-IDs");
        ode_listing_free(&location->listing);
        failed += check(!library_open(&ode,route,2,&snapshot,1,"/Storage/Alpha/Disc 1.iso",location) &&
                        location->depth == 1 && location->selected == 2 && !strcmp(currentImage.curPath,"0:\\Alpha"),
                        "favorite-opens-parent-and-selects-file");
        ode_listing_free(&location->listing); free(location);
    }
    ode_listing_free(&snapshot);
    free(currentImage.curPath);
    free(currentImage.curDir);
    return failed ? 1 : 0;
}
