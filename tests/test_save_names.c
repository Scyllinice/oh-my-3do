#include "save_names.h"
#include "usb_art.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void entry(SaveArchive *a, const char *name, const char *data)
{
    SaveEntry *e = &a->entries[a->count++];
    strcpy(e->name, name); e->size = strlen(data); e->data = (const unsigned char *)data;
}

int main(void)
{
    static const char disc[] = "# from discs\n=EA_RRX\tRoad Rash\r\n*SYNDICATE.\tSyndicate\n*SYN\tShort prefix\n"
        "=bad\n\tNo key\n=TooLongNameTooLongNameTooLongName1\tToo long\n=Shared\tDisc title";
    static const char learned[] = "=Shared\tLearned title\n";
    SaveNames names;
    SaveArchive before, after;
    char snapshot[SAVE_SNAPSHOT_BYTES], out[256];
    memset(&before, 0, sizeof(before)); memset(&after, 0, sizeof(after));

    assert(!save_names_parse(&names, disc, strlen(disc), learned, strlen(learned)));
    assert(names.count == 5);
    assert(!strcmp(save_names_find(&names, "ea_rrx"), "Road Rash"));
    assert(!save_names_find(&names, "EA_RRX2"));
    assert(!strcmp(save_names_find(&names, "SYNDICATE.03"), "Syndicate"));
    assert(!strcmp(save_names_find(&names, "SYNTH"), "Short prefix"));
    assert(!strcmp(save_names_find(&names, "Shared"), "Learned title"));
    assert(!save_names_find(&names, "bad"));

    entry(&before, "EA_RRX", "race"); entry(&before, "Stays", "same"); entry(&before, "Grows", "a");
    assert(save_snapshot_format(snapshot, 64, "Gex", 3, &before) < 0);
    assert(save_snapshot_format(snapshot, sizeof(snapshot), "Bad\tTitle", 9, &before) < 0);
    assert(!save_snapshot_format(snapshot, sizeof(snapshot), "Gex (USA)", 3, &before));
    assert(!strncmp(snapshot, "OH3DO_SAVESNAP1\nGex\nEA_RRX\t4\t", 29));

    entry(&after, "EA_RRX", "raced"); entry(&after, "Stays", "same"); entry(&after, "Grows", "ab"); entry(&after, "GEX_SAVE", "new");
    strcpy(out, "=Old\tKept\n");
    assert(save_snapshot_learn(snapshot, strlen(snapshot), &after, &names, out, sizeof(out)) == 2);
    assert(!strcmp(out, "=Old\tKept\n=Grows\tGex\n=GEX_SAVE\tGex\n"));
    /* A full learned buffer keeps what fits. */
    out[0] = 0;
    assert(save_snapshot_learn(snapshot, strlen(snapshot), &after, &names, out, 16) == 1 && !strcmp(out, "=Grows\tGex\n"));
    assert(save_snapshot_learn("OH3DO_SAVESNAP1\n\n", 17, &after, &names, out, sizeof(out)) < 0);
    assert(save_snapshot_learn("junk", 4, &after, &names, out, sizeof(out)) < 0);
    save_names_free(&names);
    assert(!save_names_parse(&names, "", 0, "", 0) && !names.count && !save_names_find(&names, "x"));
    save_names_free(&names);
    puts("Save names: disc/learned lookup, prefixes, snapshots and learning after a launch passed");
    return 0;
}
