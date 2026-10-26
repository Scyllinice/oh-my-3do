/*
 * The OM3DVID7 encoder: turns 320x240 RGB frames (and optional sound) into a
 * streamed preview file (see om3dvid7.h for the format). C99, for PCs.
 *
 * Usage:
 *   1. Allocate om3dvid7_encoder_workspace_bytes(frames) bytes and call
 *      om3dvid7_encoder_begin with them.
 *   2. Add each frame with om3dvid7_encoder_add_frame.
 *   3. Call om3dvid7_encoder_finish with the settings (start from
 *      om3dvid7_encoder_default_settings), the sound, and an output buffer.
 *
 * The caller provides all memory, so the encoder needs no C runtime (the
 * Windows DLL has none), and separate workspaces can encode on separate
 * threads at once. Encoding is deterministic: the same input always gives the
 * same file.
 */
#ifndef OM3DVID7_ENCODER_H
#define OM3DVID7_ENCODER_H
#include <stdint.h>

#if defined(_WIN32) && defined(OM3DVID7_DLL)
#define OM3DVID7_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#define OM3DVID7_EXPORT __attribute__((visibility("default")))
#else
#define OM3DVID7_EXPORT
#endif

/* The encoder's interface version, returned by om3dvid7_encoder_abi. It
 * changes whenever these functions or the settings structure change. */
#define OM3DVID7_ENCODER_ABI 2

/* What the encoder aims for. All fields are 32-bit, in this order, so other
 * languages can pass the structure directly. */
typedef struct Om3dvid7EncoderSettings {
    uint32_t rate;       /* bytes per second the file should average */
    uint32_t read_rate;  /* bytes per second the console's reader is assumed to manage */
    uint32_t slot_bytes; /* memory the player is assumed to have for chunks */
    uint32_t max_chunk;  /* largest padded chunk, a multiple of 2048 */
    uint32_t chunk_frames;
    uint32_t entries4, entries2;
    uint32_t cycle_cap; /* modelled decode clocks per frame */
} Om3dvid7EncoderSettings;

/* om3dvid7_encoder_finish's statistics, by index into its `stats` array. */
enum {
    OM3DVID7_STAT_LAMBDA_X1000,  /* the quality setting chosen, times 1000 */
    OM3DVID7_STAT_SLOWEST_FRAME, /* the slowest frame's modelled decode clocks */
    OM3DVID7_STAT_ENTRIES4,      /* 4x4 dictionary entries used */
    OM3DVID7_STAT_ENTRIES2,      /* 2x2 dictionary entries used */
    OM3DVID7_STAT_PREROLL,       /* chunks to read before playback starts */
    OM3DVID7_STAT_SLOTS,         /* chunk buffers the player is assumed to have */
    OM3DVID7_STAT_LARGEST_CHUNK, /* bytes */
    OM3DVID7_STAT_CHUNKS,
    OM3DVID7_STAT_COUNT
};

/* OM3DVID7_ENCODER_ABI, so a program loading the library can check it. */
OM3DVID7_EXPORT uint32_t om3dvid7_encoder_abi(void);

/* The settings Oh My 3DO uses: about 200 KB/s, 10-frame chunks, 640 KiB of
 * chunk memory, and frames the ARM60 decodes in under 230,000 clocks. */
OM3DVID7_EXPORT void om3dvid7_encoder_default_settings(Om3dvid7EncoderSettings *settings);

/* Workspace bytes for a clip of up to `frames` frames (0 if `frames` is 0 or
 * above OM3DVID7_MAX_FRAMES). */
OM3DVID7_EXPORT uint64_t om3dvid7_encoder_workspace_bytes(uint32_t frames);

/* Start a clip of up to `frames` frames in `workspace` (`bytes` long).
 * Returns 0, or -1 for bad arguments. */
OM3DVID7_EXPORT int32_t om3dvid7_encoder_begin(void *workspace, uint64_t bytes, uint32_t frames);

/* Add the next frame: 320x240 pixels of 8-bit RGB, in rows. Returns 0, or -1
 * if the clip is already full. */
OM3DVID7_EXPORT int32_t om3dvid7_encoder_add_frame(void *workspace, const uint8_t *rgb);

/*
 * Encode the frames added since om3dvid7_encoder_begin into `out`
 * (`capacity` bytes). `audio` is signed 8-bit mono at 11025 Hz, exactly
 * frames * 11025 / 30 samples (rounded down), or NULL for a silent clip.
 * `stats`, if not NULL, receives OM3DVID7_STAT_COUNT values (see above).
 *
 * Returns the file's length, or -1 for bad arguments, -2 if `out` is too
 * small, -3 if decoding the result disagreed with the encoder (a bug), or -4
 * if no quality setting meets both the rate and the reader model.
 */
OM3DVID7_EXPORT int64_t om3dvid7_encoder_finish(void *workspace,
                                                const Om3dvid7EncoderSettings *settings,
                                                const int8_t *audio, uint64_t audio_samples,
                                                uint8_t *out, uint64_t capacity, uint32_t *stats);

/* For tuning the dictionaries: the mean error per block of the nearest 4x4
 * entry (out[0]) and the nearest four 2x2 entries (out[1]) in the last encode. */
OM3DVID7_EXPORT void om3dvid7_encoder_debug(void *workspace, double *out);
#endif
