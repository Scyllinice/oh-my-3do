/*
 * Reading OM3DVID7 clips on a PC: decodes a whole clip held in memory, frame
 * by frame, into ordinary 8-bit RGB pictures, and gives its sound. C99, for
 * PCs and for other languages (the om3dvid7 shared library exports these).
 *
 * It uses the same decoder as the console, so the pictures are exactly what
 * a 3DO shows:
 *   OM3DVID7_SCREEN  the full 320x240 picture
 *   OM3DVID7_TV      the 160x120 picture of 2x2 averages (a menu's small TV)
 * Each call can ask for either; switching redraws the new picture whole.
 *
 * Usage:
 *   1. Allocate om3dvid7_reader_workspace_bytes() bytes (any alignment) and
 *      call om3dvid7_reader_open with them and the clip.
 *   2. Call om3dvid7_reader_next for each frame. After the last frame it
 *      starts over from the first, as players loop.
 *   3. Optionally copy the sound out with om3dvid7_reader_audio.
 *
 * The caller provides all memory and the clip must stay unchanged while it
 * is read. Separate workspaces can read on separate threads at once.
 */
#ifndef OM3DVID7_READER_H
#define OM3DVID7_READER_H
#include <stdint.h>

#ifndef OM3DVID7_EXPORT
#if defined(_WIN32) && defined(OM3DVID7_DLL)
#define OM3DVID7_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#define OM3DVID7_EXPORT __attribute__((visibility("default")))
#else
#define OM3DVID7_EXPORT
#endif
#endif

/* The reader's interface version, returned by om3dvid7_reader_abi. */
#define OM3DVID7_READER_ABI 1

/* Bytes of one RGB picture om3dvid7_reader_next writes. */
#define OM3DVID7_READER_SCREEN_RGB_BYTES (320UL * 240UL * 3UL)
#define OM3DVID7_READER_TV_RGB_BYTES     (160UL * 120UL * 3UL)

/* OM3DVID7_READER_ABI, so a program loading the library can check it. */
OM3DVID7_EXPORT uint32_t om3dvid7_reader_abi(void);

/* Workspace bytes a reader needs (a little over 600 KiB). */
OM3DVID7_EXPORT uint64_t om3dvid7_reader_workspace_bytes(void);

/*
 * Opens the clip in `data` (`size` bytes: the whole file) in `workspace`
 * (`bytes` long). Checks the header block and that the chunk table matches
 * the file; each chunk is checked when it is reached. Returns 0, or -1.
 */
OM3DVID7_EXPORT int32_t om3dvid7_reader_open(void *workspace, uint64_t bytes, const uint8_t *data,
                                             uint64_t size);

/* The clip's frame count, and its sound's sample rate (0 when silent). */
OM3DVID7_EXPORT uint32_t om3dvid7_reader_frames(const void *workspace);
OM3DVID7_EXPORT uint32_t om3dvid7_reader_audio_rate(const void *workspace);

/*
 * Decodes the next frame into `rgb`: OM3DVID7_READER_SCREEN_RGB_BYTES for
 * OM3DVID7_SCREEN, or OM3DVID7_READER_TV_RGB_BYTES for OM3DVID7_TV, in rows
 * of red, green, blue bytes. Returns the frame's number within the clip
 * (counting from 0), or -1 if the clip is damaged (the reader then stays
 * failed).
 */
OM3DVID7_EXPORT int32_t om3dvid7_reader_next(void *workspace, int32_t picture, uint8_t *rgb);

/* Starts again from the first frame. */
OM3DVID7_EXPORT void om3dvid7_reader_rewind(void *workspace);

/*
 * Copies the clip's whole sound (signed 8-bit mono at the audio rate) into
 * `out`, up to `capacity` samples, checking every chunk on the way. Returns
 * the clip's total sample count (0 when silent), or -1 if the clip is
 * damaged. `out` may be NULL to ask only for the count.
 */
OM3DVID7_EXPORT int64_t om3dvid7_reader_audio(void *workspace, int8_t *out, uint64_t capacity);
#endif
