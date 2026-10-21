#ifndef OH3DO_GAME_INFO_H
#define OH3DO_GAME_INFO_H
#include <stddef.h>
#define GAME_DESCRIPTION_BYTES 2049
/* A bounded optional text file; checksum and full path prevent collision/stale reads. */
void game_info_filename(const char *path,char *name);
int game_info_parse(const char *bytes,size_t length,const char *path,char *description,size_t capacity);
#endif
