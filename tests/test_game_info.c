#include "game_info.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv)
{
    char file[12288], description[GAME_DESCRIPTION_BYTES], name[32];
    size_t length, i;
    FILE *in;
    const char *path = "Games/USA/Example/disc.iso";
    assert(argc == 2);
    in = fopen(argv[1], "rb");
    assert(in);
    length = fread(file, 1, sizeof(file), in);
    fclose(in);
    game_info_filename(path, name);
    assert(!strcmp(name, "info/3bfadbcd.txt"));
    assert(!game_info_parse(file, length, path, description, sizeof(description)));
    assert(!strcmp(description, "Cafe - an adventure. Play & explore!"));
    assert(game_info_parse(file, length, "Games/USA/Wrong/disc.iso", description,
                           sizeof(description)) < 0 &&
           !*description);
    assert(game_info_parse(file, length, path, description, 5) < 0);
    for (i = 0; i < length; ++i) {
        assert(game_info_parse(file, i, path, description, sizeof(description)) < 0);
    }
    for (i = 0; i < length; ++i) {
        char old = file[i];
        file[i] = (char)(old ^ 1);
        assert(game_info_parse(file, length, path, description, sizeof(description)) < 0);
        file[i] = old;
    }
    puts("Game info: shared desktop fixture, path binding, checksum and bounds passed");
    return 0;
}
