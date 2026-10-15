#ifndef OH3DO_MENU_MUSIC_H
#define OH3DO_MENU_MUSIC_H
/* Audio items are 32-bit Item values on the console; no SDK types in callers. */
typedef struct MenuMusic {
    long sample, player, output, attachment;
    int opened;
    int status; /* 0 off, 1 playing, -1 unavailable (no automatic retry). */
} MenuMusic;
int menu_music_set(MenuMusic *music, int enabled);
void menu_music_close(MenuMusic *music);
#endif
