/* Headless libretro frontend for testing the menu in the Opera 3DO emulator.
 *
 *   runner CORE.so SYSTEM_DIR GAME.iso SCRIPT OUT_DIR
 *
 * The emulated FCare ODE serves the folder containing GAME.iso as its USB
 * drive. SCRIPT lines (one per line, # comments):
 *   wait N              run N frames (60 per second)
 *   press BUTTON [N]    hold BUTTON for N frames (default 4), then release 4
 *   hold BUTTON N       hold BUTTON for N frames
 *   chord B1+B2 [N]     hold several buttons together
 *   down BUTTONS / up   start holding until "up" (bursts can run in between)
 *   shot NAME           save the current frame as OUT_DIR/NAME.ppm
 *   burst NAME N STEP   N screenshots, STEP frames apart: NAME-00.ppm ...
 * Buttons are the 3DO pad's: up down left right a b c p x l r.
 */
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libretro.h"

static const char *system_dir, *out_dir;
static unsigned pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;
static unsigned char *frame;
static unsigned frame_width, frame_height;
static unsigned buttons;
static unsigned long frames_run;

static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
    va_list args;
    if (level < RETRO_LOG_WARN && !getenv("RUNNER_VERBOSE"))
        return;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

static bool environment(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = system_dir;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        pixel_format = *(const enum retro_pixel_format *)data;
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = log_cb;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *var = (struct retro_variable *)data;
        /* The console under test: an FZ-1 with its own BIOS, no HLE shortcuts. */
        if (!strcmp(var->key, "opera_bios")) {
            var->value = "panafz1.bin";
            return true;
        }
        if (!strcmp(var->key, "opera_swi_hle")) {
            var->value = "disabled";
            return true;
        }
        if (!strcmp(var->key, "opera_dsp_threaded")) {
            var->value = "disabled";
            return true;
        }
        var->value = NULL;
        return false;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false;
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_MAX_USERS:
        *(unsigned *)data = 1;
        return true;
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
        return true;
    default:
        return false;
    }
}

static void video(const void *data, unsigned width, unsigned height, size_t pitch)
{
    unsigned x, y;
    if (!data)
        return;
    if (width != frame_width || height != frame_height) {
        free(frame);
        frame = malloc((size_t)width * height * 3);
        if (!frame)
            abort();
        frame_width = width;
        frame_height = height;
    }
    for (y = 0; y < height; ++y) {
        const unsigned char *row = (const unsigned char *)data + y * pitch;
        for (x = 0; x < width; ++x) {
            unsigned char *out = frame + (y * width + x) * 3;
            if (pixel_format == RETRO_PIXEL_FORMAT_XRGB8888) {
                unsigned v = ((const unsigned *)row)[x];
                out[0] = (unsigned char)(v >> 16);
                out[1] = (unsigned char)(v >> 8);
                out[2] = (unsigned char)v;
            } else {
                unsigned v = ((const unsigned short *)row)[x];
                if (pixel_format == RETRO_PIXEL_FORMAT_RGB565) {
                    out[0] = (unsigned char)(((v >> 11) & 31) * 255 / 31);
                    out[1] = (unsigned char)(((v >> 5) & 63) * 255 / 63);
                    out[2] = (unsigned char)((v & 31) * 255 / 31);
                } else {
                    out[0] = (unsigned char)(((v >> 10) & 31) * 255 / 31);
                    out[1] = (unsigned char)(((v >> 5) & 31) * 255 / 31);
                    out[2] = (unsigned char)((v & 31) * 255 / 31);
                }
            }
        }
    }
}

static void audio_sample(int16_t left, int16_t right)
{
    (void)left;
    (void)right;
}
static size_t audio_batch(const int16_t *data, size_t frames)
{
    (void)data;
    return frames;
}
static void input_poll(void)
{
}
static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)index;
    if (port != 0 || device != RETRO_DEVICE_JOYPAD)
        return 0;
    return (buttons >> id) & 1U ? 1 : 0;
}

static int button_bit(const char *name)
{
    /* 3DO pad button -> libretro id, as Opera's lr_input.c maps them. */
    static const struct {
        const char *name;
        unsigned id;
    } map[] = {{"up", RETRO_DEVICE_ID_JOYPAD_UP},     {"down", RETRO_DEVICE_ID_JOYPAD_DOWN},
               {"left", RETRO_DEVICE_ID_JOYPAD_LEFT}, {"right", RETRO_DEVICE_ID_JOYPAD_RIGHT},
               {"a", RETRO_DEVICE_ID_JOYPAD_Y},       {"b", RETRO_DEVICE_ID_JOYPAD_B},
               {"c", RETRO_DEVICE_ID_JOYPAD_A},       {"p", RETRO_DEVICE_ID_JOYPAD_START},
               {"x", RETRO_DEVICE_ID_JOYPAD_SELECT},  {"l", RETRO_DEVICE_ID_JOYPAD_L},
               {"r", RETRO_DEVICE_ID_JOYPAD_R}};
    size_t i;
    for (i = 0; i < sizeof(map) / sizeof(map[0]); ++i)
        if (!strcmp(name, map[i].name))
            return (int)map[i].id;
    return -1;
}

static unsigned parse_buttons(const char *spec)
{
    char copy[64], *part;
    unsigned mask = 0;
    strncpy(copy, spec, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = 0;
    for (part = strtok(copy, "+"); part; part = strtok(NULL, "+")) {
        int bit = button_bit(part);
        if (bit < 0) {
            fprintf(stderr, "unknown button %s\n", part);
            exit(2);
        }
        mask |= 1U << bit;
    }
    return mask;
}

static void (*run)(void);
static void step(unsigned long count)
{
    while (count--) {
        run();
        ++frames_run;
    }
}

static void shot(const char *name)
{
    char path[1024];
    FILE *file;
    if (!frame) {
        fprintf(stderr, "no frame yet for %s\n", name);
        return;
    }
    snprintf(path, sizeof(path), "%s/%s.ppm", out_dir, name);
    file = fopen(path, "wb");
    if (!file) {
        perror(path);
        exit(1);
    }
    fprintf(file, "P6\n%u %u\n255\n", frame_width, frame_height);
    fwrite(frame, 3, (size_t)frame_width * frame_height, file);
    fclose(file);
    printf("shot %s at frame %lu (%ux%u)\n", name, frames_run, frame_width, frame_height);
}

#define LOAD(sym)                                                                                  \
    do {                                                                                           \
        *(void **)&sym##_fn = dlsym(core, #sym);                                                   \
        if (!sym##_fn) {                                                                           \
            fprintf(stderr, "missing %s\n", #sym);                                                 \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

int main(int argc, char **argv)
{
    void *core;
    void (*retro_set_environment_fn)(retro_environment_t);
    void (*retro_set_video_refresh_fn)(retro_video_refresh_t);
    void (*retro_set_audio_sample_fn)(retro_audio_sample_t);
    void (*retro_set_audio_sample_batch_fn)(retro_audio_sample_batch_t);
    void (*retro_set_input_poll_fn)(retro_input_poll_t);
    void (*retro_set_input_state_fn)(retro_input_state_t);
    void (*retro_init_fn)(void);
    bool (*retro_load_game_fn)(const struct retro_game_info *);
    void (*retro_run_fn)(void);
    void (*retro_unload_game_fn)(void);
    void (*retro_deinit_fn)(void);
    void (*retro_set_controller_port_device_fn)(unsigned, unsigned);
    struct retro_game_info game;
    char line[256];
    FILE *script;

    if (argc != 6) {
        fprintf(stderr, "usage: runner CORE.so SYSTEM_DIR GAME.iso SCRIPT OUT_DIR\n");
        return 2;
    }
    system_dir = argv[2];
    out_dir = argv[5];
    core = dlopen(argv[1], RTLD_NOW);
    if (!core) {
        fprintf(stderr, "%s\n", dlerror());
        return 1;
    }
    LOAD(retro_set_environment);
    LOAD(retro_set_video_refresh);
    LOAD(retro_set_audio_sample);
    LOAD(retro_set_audio_sample_batch);
    LOAD(retro_set_input_poll);
    LOAD(retro_set_input_state);
    LOAD(retro_init);
    LOAD(retro_load_game);
    LOAD(retro_run);
    LOAD(retro_unload_game);
    LOAD(retro_deinit);
    LOAD(retro_set_controller_port_device);
    run = retro_run_fn;
    retro_set_environment_fn(environment);
    retro_set_video_refresh_fn(video);
    retro_set_audio_sample_fn(audio_sample);
    retro_set_audio_sample_batch_fn(audio_batch);
    retro_set_input_poll_fn(input_poll);
    retro_set_input_state_fn(input_state);
    retro_init_fn();
    memset(&game, 0, sizeof(game));
    game.path = argv[3];
    if (!retro_load_game_fn(&game)) {
        fprintf(stderr, "could not load %s\n", argv[3]);
        return 1;
    }
    retro_set_controller_port_device_fn(0, RETRO_DEVICE_JOYPAD);

    script = fopen(argv[4], "r");
    if (!script) {
        perror(argv[4]);
        return 1;
    }
    while (fgets(line, sizeof(line), script)) {
        char op[32], arg[128];
        unsigned long n = 0, step_frames = 0;
        int fields;
        line[strcspn(line, "#\r\n")] = 0;
        fields = sscanf(line, "%31s %127s %lu %lu", op, arg, &n, &step_frames);
        if (fields < 1)
            continue;
        if (!strcmp(op, "wait"))
            step(strtoul(arg, NULL, 10));
        else if (!strcmp(op, "press") || !strcmp(op, "chord")) {
            buttons = parse_buttons(arg);
            step(fields >= 3 ? n : 4);
            buttons = 0;
            step(4);
        } else if (!strcmp(op, "hold")) {
            buttons = parse_buttons(arg);
            step(n);
            buttons = 0;
        } else if (!strcmp(op, "down"))
            buttons = parse_buttons(arg); /* press and keep holding */
        else if (!strcmp(op, "up"))
            buttons = 0; /* release everything */
        else if (!strcmp(op, "shot"))
            shot(arg);
        else if (!strcmp(op, "burst")) {
            unsigned long i;
            char name[160];
            for (i = 0; i < n; ++i) {
                snprintf(name, sizeof(name), "%s-%02lu", arg, i);
                shot(name);
                if (i + 1 < n)
                    step(step_frames);
            }
        } else {
            fprintf(stderr, "unknown script command %s\n", op);
            return 2;
        }
    }
    fclose(script);
    printf("ran %lu frames\n", frames_run);
    retro_unload_game_fn();
    retro_deinit_fn();
    return 0;
}
