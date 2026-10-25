/*
 * Loading covers and previews from the USB drive. See usb_media.h, and
 * docs/media-format.md for the file formats.
 *
 * The file is in four parts:
 *   1. Memory: where clip data goes on the console.
 *   2. Payload checks: the checksums that prove a file was read intact.
 *   3. Previews: loading a block-codec clip (OM3DVID5) into memory.
 *   4. Covers: PNG covers and prepared covers (OM3DCOV2).
 */
#include "usb_media.h"
#include "usb_file.h"
#include "preview_v5.h"
#include "preview_v7.h"
#include "../vendor/pngle/pngle.h"
#include <stdlib.h>
#include <string.h>
#include "../vendor/pngle/miniz.h"
#ifdef OH3DO_SDK
#include "mem.h"
#endif

/* The block decoder may read this many bytes past the end of a packet. */
#define READ_AHEAD PREVIEW_V5_READ_AHEAD

/* Files are read 2 KiB at a time, the size of the FCare's buffer. */
#define READ_BLOCK 2048

/* Preview audio: 8-bit mono samples at this rate. */
#define AUDIO_RATE 11025UL

/* The size of an OM3DVID5 header and its fields. */
#define VID5_HEADER_BYTES  40
#define VID5_TOTAL_BYTES   12
#define VID5_CHECKSUM      16
#define VID5_VIDEO_BYTES   24
#define VID5_AUDIO_BYTES   28
#define VID5_AUDIO_RATE    32
#define VID5_AUDIO_SAMPLES 36

static unsigned long read_be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) |
           p[3];
}

/* ---- 1. Memory -------------------------------------------------------- */

/*
 * The console has three kinds of memory a clip can use: main memory
 * (DRAM), video memory (VRAM) and memory the sound chip can read. Some is
 * always left free (the reserves) for the menu's own text and covers. On a
 * PC there is no VRAM; tests pretend main memory comes in fixed-size blocks,
 * like a fragmented console heap.
 */
#define DRAM_RESERVE 65536UL
#define VRAM_RESERVE 32768UL
#define HOST_CHUNK   (256UL * 1024UL)

/* Memory for preview audio, which the sound chip plays directly. */
static unsigned char *audio_memory(unsigned long bytes)
{
#ifdef OH3DO_SDK
    return (unsigned char *)AllocMem((int32)bytes, MEMTYPE_AUDIO);
#else
    return (unsigned char *)malloc((size_t)bytes);
#endif
}

/* The largest chunk that can be allocated now, after the reserve. */
static unsigned long largest_chunk(int vram)
{
#ifdef OH3DO_SDK
    MemInfo memory;
    unsigned long free_bytes, largest;
    unsigned long reserve = vram ? VRAM_RESERVE : DRAM_RESERVE;

    AvailMem(&memory, vram ? MEMTYPE_VRAM : MEMTYPE_DRAM);
    free_bytes = memory.minfo_SysFree + memory.minfo_TaskFree;
    largest = memory.minfo_SysLargest > memory.minfo_TaskLargest ? memory.minfo_SysLargest
                                                                 : memory.minfo_TaskLargest;
    if (free_bytes <= reserve)
        return 0;
    if (largest > free_bytes - reserve)
        largest = free_bytes - reserve;
    /* Leave a page spare for the allocator's rounding. */
    return largest > 4096UL ? largest - 4096UL : 0;
#else
    return vram ? 0 : HOST_CHUNK;
#endif
}

/* All the memory chunks could use, video and main memory together. */
static unsigned long total_chunk_room(void)
{
#ifdef OH3DO_SDK
    MemInfo memory;
    unsigned long total = 0, free_bytes;

    AvailMem(&memory, MEMTYPE_VRAM);
    free_bytes = memory.minfo_SysFree + memory.minfo_TaskFree;
    if (free_bytes > VRAM_RESERVE)
        total += free_bytes - VRAM_RESERVE;
    AvailMem(&memory, MEMTYPE_DRAM);
    free_bytes = memory.minfo_SysFree + memory.minfo_TaskFree;
    if (free_bytes > DRAM_RESERVE)
        total += free_bytes - DRAM_RESERVE;
    return total;
#else
    return USB_PREVIEW_CHUNKS * HOST_CHUNK;
#endif
}

/* Memory for the decoded frame, which is drawn as a CEL. */
static unsigned char *frame_memory(unsigned long bytes)
{
#ifdef OH3DO_SDK
    return (unsigned char *)AllocMem((int32)bytes, MEMTYPE_CEL);
#else
    return (unsigned char *)malloc((size_t)bytes);
#endif
}

static unsigned char *chunk_memory(unsigned long bytes, int vram)
{
#ifdef OH3DO_SDK
    return (unsigned char *)AllocMem((int32)bytes, vram ? MEMTYPE_VRAM : MEMTYPE_DRAM);
#else
    (void)vram;
    return (unsigned char *)malloc((size_t)bytes);
#endif
}

static void free_chunks(UsbPreview *preview)
{
    unsigned int i;
    for (i = 0; i < preview->chunk_count; ++i) {
#ifdef OH3DO_SDK
        FreeMem(preview->chunk[i], (int32)preview->chunk_bytes[i]);
#else
        free(preview->chunk[i]);
#endif
        preview->chunk[i] = NULL;
    }
    preview->chunk_count = 0;
    preview->vram_bytes = 0;
}

void usb_preview_free(UsbPreview *preview)
{
    free_chunks(preview);
#ifdef OH3DO_SDK
    /* Portfolio's FreeMem needs the size that was allocated. */
    if (preview->audio)
        FreeMem(preview->audio, (int32)preview->audio_bytes);
    if (preview->frame_buffer)
        FreeMem(preview->frame_buffer, (int32)preview->frame_bytes);
#else
    free(preview->audio);
    free(preview->frame_buffer);
#endif
    free(preview->table);
    free(preview->decoder);
    memset(preview, 0, sizeof(*preview));
}

int usb_preview_frame(UsbPreview *preview, unsigned int frame, unsigned char *pixels)
{
    PreviewV5 *state = (PreviewV5 *)preview->decoder;

    if (!preview->table || !state || !preview->frame_buffer || frame >= preview->frames || !pixels)
        return -1;

    /* Each packet changes the frame before it, so going back means starting
     * over: from a black frame and an empty decoder. */
    if ((unsigned long)frame + 1 < state->frames) {
        memset(state, 0, sizeof(*state));
        memset(preview->frame_buffer, 0, (size_t)preview->frame_bytes);
    }

    /* Decode forward until reaching the wanted frame. */
    while (state->frames <= frame) {
        unsigned long first = read_be32(preview->table + state->frames * 4);
        unsigned long last = read_be32(preview->table + (state->frames + 1) * 4);
        unsigned int k = 0;

        /* Find the chunk holding this packet (packets never span chunks). */
        while (k < preview->chunk_count &&
               !(first >= preview->chunk_begin[k] && last <= preview->chunk_end[k]))
            ++k;
        if (first >= last || k == preview->chunk_count)
            return -1;
        if (preview_v5_frame(state, preview->chunk[k] + (first - preview->chunk_begin[k]),
                             last - first, preview->frame_buffer) < 0)
            return -1;
    }

    if (pixels != preview->frame_buffer)
        memcpy(pixels, preview->frame_buffer, (size_t)preview->frame_bytes);
    return 0;
}

/* The path to open for a media file: "covers/NAME" (or "previews/NAME") in
 * the drive's .ohmy3do, or just NAME when the media sits beside the disc. */
static void media_path(const Ode *ode, char *path, const char *folder, const char *filename)
{
    if (ode->media_beside) {
        strcpy(path, filename);
        return;
    }
    strcpy(path, folder);
    strcat(path, "/");
    strcat(path, filename);
}

/* ---- 2. Payload checks ------------------------------------------------ */

/*
 * Covers use CRC32. Previews use a simpler word sum, about ten times cheaper
 * on the ARM60 (CRC32 took longer than reading the data over USB): over the
 * payload's big-endian 32-bit words, a += word; b += a; and the stored value
 * is a XOR (b rotated by 16 bits), all modulo 2^32. A preview's payload is
 * padded to whole words, so every read is word-aligned.
 */
typedef struct PayloadCheck {
    int word_sum; /* 1: the preview word sum; 0: CRC32 */
    int bad;      /* a read was not a whole number of words */
    unsigned long crc, a, b;
} PayloadCheck;

static void check_start(PayloadCheck *check, int word_sum)
{
    memset(check, 0, sizeof(*check));
    check->word_sum = word_sum;
    check->crc = 0xffffffffUL;
}

static void check_update(PayloadCheck *check, const unsigned char *data, size_t bytes)
{
    unsigned long a = check->a, b = check->b;
    size_t words = bytes >> 2;

    if (!check->word_sum) {
        check->crc = usb_art_crc32_update(check->crc, data, bytes);
        return;
    }
    if (bytes & 3U)
        check->bad = 1;

#ifdef __BIG_ENDIAN
    /*
     * On the (big-endian) console, read aligned data a word at a time, four
     * words per loop pass: the console's own byte order already matches the
     * file's, so no bytes need shuffling.
     */
    if (!((unsigned long)data & 3UL)) {
        const unsigned long *word = (const unsigned long *)(const void *)data;
        while (words >= 4) {
            a += word[0];
            b += a;
            a += word[1];
            b += a;
            a += word[2];
            b += a;
            a += word[3];
            b += a;
            word += 4;
            words -= 4;
        }
        while (words--) {
            a += *word++;
            b += a;
        }
        check->a = a;
        check->b = b;
        return;
    }
#endif

    /* Anywhere else, assemble each word from its bytes. */
    while (words--) {
        a += read_be32(data);
        b += a;
        data += 4;
    }
    check->a = a;
    check->b = b;
}

/* Whether the data checked so far matches the file's stored value. */
static int check_matches(const PayloadCheck *check, unsigned long expected)
{
    unsigned long a = check->a & 0xffffffffUL;
    unsigned long b = check->b & 0xffffffffUL;

    if (!check->word_sum)
        return (check->crc ^ 0xffffffffUL) == expected;
    return !check->bad && ((a ^ ((b << 16) | (b >> 16))) & 0xffffffffUL) == expected;
}

/*
 * Reads `bytes` of the open file into `data`, adding them to `check`. The
 * checksum is verified afterwards, so the reads clear nothing (see
 * USB_FILE_CRC_CHECKED). Stops with -1 if canceled or on failure.
 */
static int read_checked(Ode *ode, OdeReadBuffer reader, unsigned char *data, unsigned long bytes,
                        PayloadCheck *check, MediaCancel cancel, void *context)
{
    unsigned long done = 0;
    while (done < bytes) {
        size_t chunk = bytes - done > READ_BLOCK ? READ_BLOCK : (size_t)(bytes - done);
        if ((cancel && cancel(context)) ||
            usb_file_read(ode, reader, data + done, chunk, USB_FILE_CRC_CHECKED) < 0)
            return -1;
        check_update(check, data + done, chunk);
        done += chunk;
    }
    return 0;
}

/* ---- 3. Previews ------------------------------------------------------ */

/* Bytes of audio for `frames` video frames at `fps`, rounded up to whole
 * samples and then to whole words. */
static unsigned long audio_bytes_for(unsigned long frames, unsigned int fps)
{
    return (((frames * AUDIO_RATE + fps - 1) / fps) + 3) & ~3UL;
}

/*
 * Reads `source_bytes` of the open file, keeping the first `keep_bytes` in
 * `out` (which may be NULL when nothing is kept). The rest is only checked,
 * so the checksum still covers the whole file.
 */
static int read_keeping_start(Ode *ode, OdeReadBuffer reader, unsigned char *out,
                              unsigned long source_bytes, unsigned long keep_bytes,
                              PayloadCheck *check, MediaCancel cancel, void *context)
{
    unsigned long scratch_words[READ_BLOCK / 4]; /* word-aligned scratch space */
    unsigned char *scratch = (unsigned char *)scratch_words;
    unsigned long done = 0;

    while (done < source_bytes) {
        size_t chunk =
            source_bytes - done > READ_BLOCK ? READ_BLOCK : (size_t)(source_bytes - done);
        size_t kept = 0;
        if (done < keep_bytes)
            kept = keep_bytes - done < chunk ? (size_t)(keep_bytes - done) : chunk;

        if ((cancel && cancel(context)) ||
            usb_file_read(ode, reader, scratch, chunk, USB_FILE_CRC_CHECKED) < 0)
            return -1;
        check_update(check, scratch, chunk);
        if (kept)
            memcpy(out + done, scratch, kept);
        done += chunk;
    }
    return 0;
}

/*
 * Reads the next 2 KiB of video into the chunks, which hold consecutive
 * ranges of the video. Bytes past the frames that fitted are only checked.
 */
static int read_video_block(Ode *ode, OdeReadBuffer reader, UsbPreview *clip, PayloadCheck *check,
                            MediaCancel cancel, void *context)
{
    unsigned long scratch_words[READ_BLOCK / 4];
    unsigned char *scratch = (unsigned char *)scratch_words;
    unsigned long position = clip->stream_pos;
    unsigned long kept_end = clip->video_bytes;
    size_t chunk = clip->stream_end - position > READ_BLOCK ? READ_BLOCK
                                                            : (size_t)(clip->stream_end - position);
    size_t copied = 0;
    unsigned int k = clip->stream_chunk;

    if ((cancel && cancel(context)) ||
        usb_file_read(ode, reader, scratch, chunk, USB_FILE_CRC_CHECKED) < 0)
        return -1;
    check_update(check, scratch, chunk);

    /* Copy what is kept into the right chunk(s); a block can span two. */
    while (copied < chunk && position + copied < kept_end) {
        unsigned long at = position + copied;
        size_t take = chunk - copied;
        while (at >= clip->chunk_end[k])
            ++k;
        if (take > clip->chunk_end[k] - at)
            take = (size_t)(clip->chunk_end[k] - at);
        memcpy(clip->chunk[k] + (at - clip->chunk_begin[k]), scratch + copied, take);
        copied += take;
    }

    clip->stream_pos = position + chunk;
    clip->stream_chunk = k;
    return 0;
}

/*
 * Allocates memory for as much of the clip as fits, then reads it.
 *
 * Planning: keep the longest run of frames from the start that fits in
 * memory, with its audio, in up to USB_PREVIEW_CHUNKS chunks. Each chunk
 * holds whole packets plus READ_AHEAD spare bytes, so a packet never spans
 * two chunks. Video memory is used first; audio is allocated before the
 * chunks take main memory.
 *
 * Reading: the audio comes first in the file (after the frame table), then
 * the video, read to the end even past the frames kept so the checksum
 * covers everything.
 */
static int store_preview(Ode *ode, OdeReadBuffer reader, UsbPreview *clip,
                         unsigned long source_video, unsigned long source_audio, int sound,
                         PayloadCheck *check, MediaCancel cancel, void *context,
                         MediaReclaim reclaim, void *reclaim_context)
{
    unsigned long capacity = total_chunk_room();
    unsigned int frame, planned_frames = 0, placed = 0, k = 0;
    int with_audio = source_audio && sound;
    unsigned long full_audio = with_audio ? audio_bytes_for(clip->source_frames, clip->fps) : 0;
    unsigned long full_need = read_be32(clip->table + clip->source_frames * 4) - clip->table_bytes +
                              full_audio + USB_PREVIEW_CHUNKS * READ_AHEAD;

    /* Optional caches must never cut a preview short: free them if needed. */
    if (full_need > capacity && reclaim && reclaim(reclaim_context))
        capacity = total_chunk_room();

    /* How many frames (with their audio) fit? */
    for (frame = 1; frame <= clip->source_frames; ++frame) {
        unsigned long audio = with_audio ? audio_bytes_for(frame, clip->fps) : 0;
        unsigned long need = read_be32(clip->table + frame * 4) - clip->table_bytes + audio +
                             USB_PREVIEW_CHUNKS * READ_AHEAD;
        if (need <= capacity)
            planned_frames = frame;
    }
    if (!planned_frames)
        return USB_PREVIEW_NO_MEMORY;

    if (with_audio) {
        clip->audio_bytes = clip->audio_frames = audio_bytes_for(planned_frames, clip->fps);
        clip->audio_rate = AUDIO_RATE;
        clip->audio = audio_memory(clip->audio_bytes);
        if (!clip->audio && reclaim && reclaim(reclaim_context))
            clip->audio = audio_memory(clip->audio_bytes);
        if (!clip->audio)
            return USB_PREVIEW_NO_MEMORY;
    }

allocate_chunks:
    /* Fill chunk after chunk with as many whole packets as fit. */
    while (placed < planned_frames && k < USB_PREVIEW_CHUNKS) {
        unsigned long vram_room = largest_chunk(1);
        unsigned long begin = read_be32(clip->table + placed * 4);
        unsigned long next_packet_end = read_be32(clip->table + (placed + 1) * 4);
        int vram = vram_room >= next_packet_end - begin + READ_AHEAD;
        unsigned long room = vram ? vram_room : largest_chunk(0);
        unsigned int last = placed;
        unsigned long bytes;

        while (last < planned_frames &&
               read_be32(clip->table + (last + 1) * 4) - begin + READ_AHEAD <= room)
            ++last;
        if (last == placed)
            break; /* not even one more packet fits */

        bytes = read_be32(clip->table + last * 4) - begin + READ_AHEAD;
        clip->chunk[k] = chunk_memory(bytes, vram);
        if (!clip->chunk[k])
            break;
        memset(clip->chunk[k] + bytes - READ_AHEAD, 0, READ_AHEAD);
        clip->chunk_bytes[k] = bytes;
        clip->chunk_vram[k] = (unsigned char)vram;
        clip->chunk_begin[k] = begin;
        clip->chunk_end[k] = read_be32(clip->table + last * 4);
        if (vram)
            clip->vram_bytes += bytes;
        clip->chunk_count = ++k;
        placed = last;
    }
    if (placed < planned_frames && reclaim && reclaim(reclaim_context)) {
        /* Memory was too fragmented: free the caches and plan again. Nothing
         * has been read from the file yet, so this repeats no USB command. */
        free_chunks(clip);
        placed = 0;
        k = 0;
        goto allocate_chunks;
    }
    if (!placed)
        return USB_PREVIEW_NO_MEMORY;

    clip->frames = placed;
    clip->video_bytes = read_be32(clip->table + placed * 4);
    if (clip->audio)
        clip->audio_frames = audio_bytes_for(placed, clip->fps);

    /* The audio follows the frame table, so it is all read before any video. */
    if (read_keeping_start(ode, reader, clip->audio, source_audio, clip->audio_frames, check,
                           cancel, context) < 0)
        return -1;

    /* The video, to the end of the file. */
    clip->stream_pos = clip->table_bytes;
    clip->stream_end = source_video;
    clip->stream_chunk = 0;
    while (clip->stream_pos < clip->stream_end) {
        if (read_video_block(ode, reader, clip, check, cancel, context) < 0)
            return -1;
    }
    return 0;
}

/*
 * Reads a streamed clip's first 2 KiB (whose first 40 bytes are already in
 * `header`) and checks its header, for preview_stream_open. Returns 0, or -1.
 */
static int prepare_streamed_clip(Ode *ode, OdeReadBuffer reader, const unsigned char *header,
                                 UsbPreview *clip)
{
    Preview7Header parsed;

    memcpy(clip->stream_block, header, VID5_HEADER_BYTES);
    if (usb_file_read(ode, reader, clip->stream_block + VID5_HEADER_BYTES,
                      sizeof(clip->stream_block) - VID5_HEADER_BYTES, USB_FILE_CRC_CHECKED) < 0 ||
        preview7_header(clip->stream_block, &parsed))
        return -1;
    clip->format = 7;
    clip->width = PREVIEW7_WIDTH;
    clip->height = PREVIEW7_HEIGHT;
    clip->fps = PREVIEW7_FPS;
    clip->frames = clip->source_frames = (unsigned int)parsed.frames;
    return 0;
}

int usb_preview_load_reclaim(Ode *ode, OdeReadBuffer reader, const char *filename,
                             UsbPreview *preview, MediaCancel cancel, void *context,
                             MediaLoadPhase phase, void *phase_context, int sound,
                             MediaReclaim reclaim, void *reclaim_context)
{
    unsigned char header[VID5_HEADER_BYTES];
    UsbPreview clip;
    unsigned long total_bytes, source_video, source_audio, source_rate, source_samples, padding;
    unsigned long expected_audio, memory_needed;
    unsigned long decoder_bytes = sizeof(PreviewV5);
    PayloadCheck check;
    unsigned int frame;
    char path[128];
    int result = -1;

    memset(&clip, 0, sizeof(clip));
    if (!usb_media_filename(filename) || menu_path_compare(filename + strlen(filename) - 4, ".3dv"))
        return -1;
    media_path(ode, path, "previews", filename);
    if (usb_file_open(ode, path) < 0)
        return -1;
    if (usb_file_read(ode, reader, header, VID5_HEADER_BYTES, 1) < 0)
        goto done;

    /* A streamed clip: hand back its first block; the player reads the rest. */
    if (!memcmp(header, "OM3DVID7", 8)) {
        if (prepare_streamed_clip(ode, reader, header, &clip) == 0)
            result = USB_PREVIEW_STREAM;
        goto done;
    }

    /* A block-codec clip: check the header (see docs/media-format.md). */
    if (memcmp(header, "OM3DVID5", 8))
        goto done;
    clip.format = 5;
    check_start(&check, 1);
    clip.width = header[8] * 256U + header[9];
    clip.height = header[10] * 256U + header[11];
    clip.frames = clip.source_frames = header[20] * 256U + header[21];
    clip.fps = header[22] * 256U + header[23];
    total_bytes = read_be32(header + VID5_TOTAL_BYTES);
    if (clip.width != PREVIEW_V5_WIDTH || clip.height != PREVIEW_V5_HEIGHT || clip.fps != 30 ||
        !clip.frames || clip.frames > USB_PREVIEW_MAX_FRAMES || total_bytes > USB_PREVIEW_MAX_BYTES)
        goto done;
    clip.frame_bytes = (unsigned long)clip.width * clip.height * 2UL;
    source_video = read_be32(header + VID5_VIDEO_BYTES);
    source_audio = read_be32(header + VID5_AUDIO_BYTES);
    source_rate = read_be32(header + VID5_AUDIO_RATE);
    source_samples = read_be32(header + VID5_AUDIO_SAMPLES);
    clip.table_bytes = (clip.source_frames + 1) * 4UL;

    /* The payload is the video (table and packets), the audio, and zero
     * padding to a word boundary. */
    padding = (4UL - (source_video & 3UL)) & 3UL;
    if (source_video + padding > total_bytes ||
        source_audio != total_bytes - source_video - padding || source_video <= clip.table_bytes)
        goto done;
    expected_audio = audio_bytes_for(clip.source_frames, clip.fps);
    if (source_audio ? (source_rate != AUDIO_RATE || source_audio != source_samples ||
                        source_samples != expected_audio)
                     : (source_rate || source_samples))
        goto done;

    /* Free optional caches first if the whole clip and its buffers would not fit. */
    memory_needed = source_video + (source_audio && sound ? source_audio : 0) + clip.frame_bytes +
                    decoder_bytes + USB_PREVIEW_CHUNKS * READ_AHEAD;
    if (memory_needed > total_chunk_room() && reclaim)
        reclaim(reclaim_context);

    /* The decoder, the frame and the frame table, each tried again after
     * reclaiming if the first attempt fails. */
    clip.decoder = calloc(1, (size_t)decoder_bytes);
    if (!clip.decoder && reclaim && reclaim(reclaim_context))
        clip.decoder = calloc(1, (size_t)decoder_bytes);
    clip.frame_buffer = frame_memory(clip.frame_bytes);
    if (!clip.frame_buffer && reclaim && reclaim(reclaim_context))
        clip.frame_buffer = frame_memory(clip.frame_bytes);
    clip.table = (unsigned char *)malloc((size_t)clip.table_bytes);
    if (!clip.table && reclaim && reclaim(reclaim_context))
        clip.table = (unsigned char *)malloc((size_t)clip.table_bytes);
    if (!clip.decoder || !clip.frame_buffer || !clip.table) {
        result = USB_PREVIEW_NO_MEMORY;
        goto done;
    }

    /* The frame table: offsets rising from just after the table to the end
     * of the video. */
    if (read_checked(ode, reader, clip.table, clip.table_bytes, &check, cancel, context) < 0)
        goto done;
    if (read_be32(clip.table) != clip.table_bytes ||
        read_be32(clip.table + clip.source_frames * 4) != source_video)
        goto done;
    for (frame = 0; frame < clip.source_frames; ++frame) {
        unsigned long begin = read_be32(clip.table + frame * 4);
        unsigned long end = read_be32(clip.table + (frame + 1) * 4);
        if (begin < clip.table_bytes || begin >= end || end > source_video)
            goto done;
    }

    memset(clip.frame_buffer, 0, (size_t)clip.frame_bytes);
    result = store_preview(ode, reader, &clip, source_video + padding, source_audio, sound, &check,
                           cancel, context, reclaim, reclaim_context);
    if (result)
        goto done;

    /* Everything read: check the payload, find the footer exactly where it
     * belongs, and decode the first frame. */
    result = -1;
    if (phase)
        phase(phase_context, 1);
    if ((cancel && cancel(context)) || !check_matches(&check, read_be32(header + VID5_CHECKSUM)) ||
        usb_file_verify(ode, reader, (const unsigned char *)"END3DV05", 8) < 0 ||
        usb_preview_frame(&clip, 0, clip.frame_buffer) < 0)
        goto done;
    result = 0;

done:
    if (usb_file_close(ode) < 0)
        result = -1;
    if (phase)
        phase(phase_context, 2);
    if (result == 0 || result == USB_PREVIEW_STREAM) {
        /* Replace the old clip; `clip` then holds nothing to free. */
        usb_preview_free(preview);
        *preview = clip;
        memset(&clip, 0, sizeof(clip));
    }
    usb_preview_free(&clip);
    return result;
}

int usb_preview_load_options(Ode *ode, OdeReadBuffer reader, const char *filename,
                             UsbPreview *preview, MediaCancel cancel, void *context,
                             MediaLoadPhase phase, void *phase_context, int sound)
{
    return usb_preview_load_reclaim(ode, reader, filename, preview, cancel, context, phase,
                                    phase_context, sound, NULL, NULL);
}

int usb_preview_load_profiled(Ode *ode, OdeReadBuffer reader, const char *filename,
                              UsbPreview *preview, MediaCancel cancel, void *context,
                              MediaLoadPhase phase, void *phase_context)
{
    return usb_preview_load_options(ode, reader, filename, preview, cancel, context, phase,
                                    phase_context, 1);
}

int usb_preview_load(Ode *ode, OdeReadBuffer reader, const char *filename, UsbPreview *preview,
                     MediaCancel cancel, void *context)
{
    return usb_preview_load_options(ode, reader, filename, preview, cancel, context, NULL, NULL, 1);
}

/* ---- 4. Covers -------------------------------------------------------- */

/* The largest PNG the menu decodes, in each direction. */
#define PNG_MAX_SIDE 1024

/* The PNG file signature, and the size of the signature plus the IHDR chunk. */
#define PNG_SIGNATURE   "\211PNG\r\n\032\n"
#define PNG_FIRST_BYTES 33

/* Room for data the PNG decoder has not consumed yet, plus one more read. */
#define PNG_PENDING_BYTES (READ_BLOCK + 16)

/* The background color around a letterboxed cover, as RGB555 bytes. */
#define COVER_BACKGROUND_HIGH 0x04
#define COVER_BACKGROUND_LOW  0x44

/*
 * Where a PNG is drawn: the target picture, the scaling from PNG pixels to
 * target pixels, and state for canceling. Pngle (the PNG decoder) calls
 * png_init with the image size, png_draw for each decoded pixel (or block of
 * pixels, for interlaced images) and png_done at the end.
 */
typedef struct PngCanvas {
    unsigned char *pixels; /* the target: target_width x target_height RGB555 pixels */
    /* For each source column (row), the first target column (row) it covers. */
    unsigned short x_bounds[PNG_MAX_SIDE + 1], y_bounds[PNG_MAX_SIDE + 1];
    unsigned int width, height; /* the scaled image, fitted inside the target */
    unsigned int left, top;     /* where it starts, to center it */
    unsigned int target_width, target_height;
    unsigned int source_width, source_height;
    /* A small image (no bigger than the target) is decoded here at its own
     * size, then enlarged smoothly in png_done; larger ones are shrunk as
     * they are drawn. */
    unsigned char *small;
    int complete;          /* png_done ran */
    int native;            /* exactly the target size: drawn straight in */
    unsigned int rendered; /* draw calls so far, to check for canceling */
    MediaCancel cancel;
    void *context;
} PngCanvas;

/* Converts 8-bit red, green and blue to the console's 15-bit RGB555. */
static unsigned int rgb555(unsigned int red, unsigned int green, unsigned int blue)
{
    return ((red >> 3) << 10) | ((green >> 3) << 5) | (blue >> 3);
}

/* Stores one RGB555 pixel, big-endian, at `pixel`. */
static void put_pixel(unsigned char *pixel, unsigned int color)
{
    pixel[0] = (unsigned char)(color >> 8);
    pixel[1] = (unsigned char)color;
}

static void png_init(pngle_t *png, uint32_t width, uint32_t height)
{
    PngCanvas *canvas = (PngCanvas *)pngle_get_user_data(png);
    unsigned int i;

    canvas->source_width = width;
    canvas->source_height = height;
    canvas->native = width == canvas->target_width && height == canvas->target_height;

    /* Fit inside the target, keeping the image's shape. */
    canvas->width = canvas->target_width;
    canvas->height = (unsigned int)(height * canvas->target_width / width);
    if (canvas->height > canvas->target_height) {
        canvas->height = canvas->target_height;
        canvas->width = (unsigned int)(width * canvas->target_height / height);
    }

    if (width <= canvas->target_width && height <= canvas->target_height && !canvas->native) {
        canvas->small = (unsigned char *)calloc(width * height, 2);
        if (!canvas->small) {
            pngle_abort(png);
            return;
        }
    }
    if (!canvas->width)
        canvas->width = 1;
    if (!canvas->height)
        canvas->height = 1;
    canvas->left = (canvas->target_width - canvas->width) / 2;
    canvas->top = (canvas->target_height - canvas->height) / 2;

    /* Source column i covers target columns x_bounds[i] to x_bounds[i + 1]. */
    for (i = 0; i <= width; ++i)
        canvas->x_bounds[i] = (unsigned short)((i * canvas->width + width - 1) / width);
    for (i = 0; i <= height; ++i)
        canvas->y_bounds[i] = (unsigned short)((i * canvas->height + height - 1) / height);
}

static void png_draw(pngle_t *png, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                     const uint8_t rgba[4])
{
    PngCanvas *canvas = (PngCanvas *)pngle_get_user_data(png);
    unsigned int dx, dy, color, alpha = rgba[3];

    /* Every 4096 pixels, see whether the person has moved on. */
    if (!(++canvas->rendered & 4095U) && canvas->cancel && canvas->cancel(canvas->context)) {
        pngle_abort(png);
        return;
    }
    if (x + w > canvas->source_width || y + h > canvas->source_height || x + w > PNG_MAX_SIDE ||
        y + h > PNG_MAX_SIDE)
        return;
    /* Shrinking: pixels that land on no target pixel are skipped. */
    if (!canvas->small && (canvas->x_bounds[x] == canvas->x_bounds[x + w] ||
                           canvas->y_bounds[y] == canvas->y_bounds[y + h]))
        return;

    /* Transparent parts are blended over the dark background color
     * (8, 16, 33 in 8-bit RGB). */
    if (alpha == 255) {
        color = rgb555(rgba[0], rgba[1], rgba[2]);
    } else {
        unsigned int red = (rgba[0] * alpha + 8 * (255 - alpha) + 127) / 255;
        unsigned int green = (rgba[1] * alpha + 16 * (255 - alpha) + 127) / 255;
        unsigned int blue = (rgba[2] * alpha + 33 * (255 - alpha) + 127) / 255;
        color = rgb555(red, green, blue);
    }

    if (canvas->small) {
        /* Collect at the image's own size; png_done enlarges it. */
        for (dy = y; dy < y + h; ++dy) {
            for (dx = x; dx < x + w; ++dx)
                put_pixel(canvas->small + (dy * canvas->source_width + dx) * 2, color);
        }
        return;
    }

    if (canvas->native && w == 1 && h == 1) {
        /* Exactly the right size: straight in. */
        if (x >= canvas->target_width || y >= canvas->target_height)
            return;
        put_pixel(canvas->pixels + (y * canvas->target_width + x) * 2, color);
        return;
    }

    /* Shrinking: fill the target pixels this source pixel covers. */
    for (dy = canvas->y_bounds[y]; dy < canvas->y_bounds[y + h]; ++dy) {
        for (dx = canvas->x_bounds[x]; dx < canvas->x_bounds[x + w]; ++dx) {
            unsigned int position =
                ((dy + canvas->top) * canvas->target_width + dx + canvas->left) * 2;
            put_pixel(canvas->pixels + position, color);
        }
    }
}

/* One pixel of the small image collected by png_draw. */
static unsigned int small_pixel(const PngCanvas *canvas, unsigned int x, unsigned int y)
{
    unsigned int position = (y * canvas->source_width + x) * 2;
    return ((unsigned int)canvas->small[position] << 8) | canvas->small[position + 1];
}

/*
 * Enlarges a small image into the target with bilinear filtering: each
 * target pixel blends the four nearest source pixels by distance. Positions
 * use 8 bits of fraction (so 256 means one whole pixel). Returns 0, or -1
 * if canceled.
 */
static int enlarge_small_image(PngCanvas *canvas)
{
    unsigned int x, y;

    if (!canvas->small)
        return 0;

    for (y = 0; y < canvas->height; ++y) {
        /* This row's position in the source, and the two rows around it. */
        unsigned long source_y =
            canvas->height > 1 ? (y * (canvas->source_height - 1) * 256UL) / (canvas->height - 1)
                               : 0;
        unsigned int y0 = (unsigned int)(source_y >> 8);
        unsigned int y1 = y0 + 1 < canvas->source_height ? y0 + 1 : y0;
        unsigned int fraction_y = (unsigned int)(source_y & 255);

        if (!(y & 7U) && canvas->cancel && canvas->cancel(canvas->context))
            return -1;

        for (x = 0; x < canvas->width; ++x) {
            unsigned long source_x =
                canvas->width > 1 ? (x * (canvas->source_width - 1) * 256UL) / (canvas->width - 1)
                                  : 0;
            unsigned int x0 = (unsigned int)(source_x >> 8);
            unsigned int x1 = x0 + 1 < canvas->source_width ? x0 + 1 : x0;
            unsigned int fraction_x = (unsigned int)(source_x & 255);
            unsigned int corners[4], channel, color = 0;

            corners[0] = small_pixel(canvas, x0, y0);
            corners[1] = small_pixel(canvas, x1, y0);
            corners[2] = small_pixel(canvas, x0, y1);
            corners[3] = small_pixel(canvas, x1, y1);

            /* Blend blue, green and red (5 bits each) separately. */
            for (channel = 0; channel < 3; ++channel) {
                unsigned int shift = channel * 5;
                unsigned long top = ((corners[0] >> shift) & 31) * (256UL - fraction_x) +
                                    ((corners[1] >> shift) & 31) * fraction_x;
                unsigned long bottom = ((corners[2] >> shift) & 31) * (256UL - fraction_x) +
                                       ((corners[3] >> shift) & 31) * fraction_x;
                unsigned int value =
                    (unsigned int)((top * (256 - fraction_y) + bottom * fraction_y + 32768UL) >>
                                   16);
                color |= value << shift;
            }
            put_pixel(canvas->pixels +
                          ((y + canvas->top) * canvas->target_width + x + canvas->left) * 2,
                      color);
        }
    }
    return 0;
}

static void png_done(pngle_t *png)
{
    PngCanvas *canvas = (PngCanvas *)pngle_get_user_data(png);
    if (enlarge_small_image(canvas) < 0) {
        pngle_abort(png);
        return;
    }
    canvas->complete = 1;
}

/*
 * Gives `bytes` more of the file to the PNG decoder. Pngle may not use all
 * of what it is given; the rest waits in `pending` (`*used` bytes) for the
 * next call. Returns 0, or -1 if the decoder failed.
 */
static int png_feed(pngle_t *png, unsigned char *pending, size_t *used, const unsigned char *data,
                    size_t bytes)
{
    int consumed;

    if (*used + bytes > PNG_PENDING_BYTES)
        return -1;
    memcpy(pending + *used, data, bytes);
    *used += bytes;
    consumed = pngle_feed(png, pending, *used);
    if (consumed < 0 || (size_t)consumed > *used)
        return -1;
    *used -= (size_t)consumed;
    memmove(pending, pending + consumed, *used);
    return 0;
}

/* Whether a PNG chunk's type (4 bytes) is acceptable: letters only, safe to
 * copy, and either optional or one of the chunks a cover needs. */
static int png_chunk_allowed(const unsigned char *type)
{
    size_t i;
    for (i = 0; i < 4; ++i) {
        if (!((type[i] >= 'A' && type[i] <= 'Z') || (type[i] >= 'a' && type[i] <= 'z')))
            return 0;
    }
    /* The third letter's case bit is reserved and must be clear. */
    if (type[2] & 32)
        return 0;
    /* A lower-case first letter marks an optional chunk the decoder may skip. */
    if (type[0] & 32)
        return 1;
    return !memcmp(type, "IHDR", 4) || !memcmp(type, "PLTE", 4) || !memcmp(type, "IDAT", 4) ||
           !memcmp(type, "IEND", 4);
}

/*
 * Decodes the PNG cover `filename` into `pixels` (width x height), scaled to
 * fit and centered on a dark background. The file is read chunk by chunk,
 * checking each chunk's header before passing it on. Returns 0, or -1.
 */
static int decode_png_cover(Ode *ode, OdeReadBuffer reader, const char *filename,
                            unsigned char *pixels, MediaCancel cancel, void *context,
                            unsigned int width, unsigned int height)
{
    /* The CRC that ends every PNG: the IEND chunk has no data, so it is fixed. */
    static const unsigned char iend_crc[4] = {0xae, 0x42, 0x60, 0x82};
    unsigned char block[READ_BLOCK], pending[PNG_PENDING_BYTES];
    char path[128];
    pngle_t *png = NULL;
    PngCanvas *canvas = NULL;
    unsigned long bytes_read = PNG_FIRST_BYTES;
    size_t used = 0, i;
    int result = -1;

    if (!usb_media_filename(filename) || menu_path_compare(filename + strlen(filename) - 4, ".png"))
        return -1;
    media_path(ode, path, "covers", filename);
    if (usb_file_open(ode, path) < 0)
        return -1;

    /* The signature and the IHDR chunk: a size within limits and no
     * interlacing method beyond 1 (Adam7). */
    if (usb_file_read(ode, reader, block, PNG_FIRST_BYTES, 1) < 0 ||
        memcmp(block, PNG_SIGNATURE, 8) || read_be32(block + 8) != 13 ||
        memcmp(block + 12, "IHDR", 4) || !read_be32(block + 16) || !read_be32(block + 20) ||
        read_be32(block + 16) > PNG_MAX_SIDE || read_be32(block + 20) > PNG_MAX_SIDE ||
        block[28] > 1)
        goto done;

    png = pngle_new();
    canvas = (PngCanvas *)calloc(1, sizeof(*canvas));
    if (!png || !canvas)
        goto done;
    canvas->pixels = pixels;
    canvas->cancel = cancel;
    canvas->context = context;
    canvas->target_width = width;
    canvas->target_height = height;
    for (i = 0; i < width * height * 2; i += 2) {
        pixels[i] = COVER_BACKGROUND_HIGH;
        pixels[i + 1] = COVER_BACKGROUND_LOW;
    }
    pngle_set_user_data(png, canvas);
    pngle_set_init_callback(png, png_init);
    pngle_set_draw_callback(png, png_draw);
    pngle_set_done_callback(png, png_done);
    if (png_feed(png, pending, &used, block, PNG_FIRST_BYTES) < 0)
        goto done;

    /* Each chunk: an 8-byte header (length and type), its data, a 4-byte CRC. */
    while (!canvas->complete) {
        unsigned long remaining;
        int is_end;

        if ((cancel && cancel(context)) || bytes_read > USB_PNG_MAX_BYTES - 12 ||
            usb_file_read(ode, reader, block, 8, 1) < 0)
            goto done;
        if (!png_chunk_allowed(block + 4))
            goto done;
        remaining = read_be32(block);
        is_end = !memcmp(block + 4, "IEND", 4);
        bytes_read += 12;
        if (remaining > USB_PNG_MAX_BYTES - bytes_read ||
            png_feed(png, pending, &used, block, 8) < 0)
            goto done;
        bytes_read += remaining;

        while (remaining) {
            size_t chunk = remaining > READ_BLOCK ? READ_BLOCK : (size_t)remaining;
            if ((cancel && cancel(context)) || usb_file_read(ode, reader, block, chunk, 0) < 0 ||
                png_feed(png, pending, &used, block, chunk) < 0)
                goto done;
            remaining -= chunk;
        }

        if (is_end) {
            /*
             * The final CRC is known in advance, so verify it with the
             * opposite bytes placed in the buffer first (usb_file_verify): a
             * file cut short can never pass as complete.
             */
            if (usb_file_verify(ode, reader, iend_crc, 4) < 0 ||
                png_feed(png, pending, &used, iend_crc, 4) < 0)
                goto done;
        } else if (usb_file_read(ode, reader, block, 4, 1) < 0 ||
                   png_feed(png, pending, &used, block, 4) < 0) {
            goto done;
        }
    }
    /* The decoder used every byte. */
    if (!used)
        result = 0;

done:
    pngle_destroy(png);
    if (canvas)
        free(canvas->small);
    free(canvas);
    if (usb_file_close(ode) < 0)
        result = -1;
    return result;
}

int usb_png_cover(Ode *ode, OdeReadBuffer reader, const char *filename, unsigned char *pixels,
                  MediaCancel cancel, void *context)
{
    return decode_png_cover(ode, reader, filename, pixels, cancel, context, USB_COVER_WIDTH,
                            USB_COVER_HEIGHT);
}

/*
 * Loads a prepared cover (OM3DCOV2): exactly width x height RGB555 pixels,
 * zlib-compressed by the desktop app, so the console skips PNG's filters,
 * scaling and per-pixel callbacks. Returns 0, or -1.
 */
static int load_prepared_cover(Ode *ode, OdeReadBuffer reader, const char *folder,
                               const char *filename, unsigned char *pixels, int width, int height,
                               MediaCancel cancel, void *context)
{
    unsigned char header[24];
    unsigned char *compressed = NULL;
    tinfl_decompressor *decompressor = NULL;
    unsigned long compressed_bytes;
    unsigned long pixel_bytes = (unsigned long)width * height * 2;
    PayloadCheck check;
    size_t in_bytes, out_bytes = (size_t)pixel_bytes;
    tinfl_status status;
    char path[128];
    int result = -1;

    if (!usb_media_filename(filename))
        return -1;
    media_path(ode, path, folder, filename);
    if (usb_file_open(ode, path) < 0)
        return -1;

    /* Header: magic, size (16-bit width and height), compressed size, CRC,
     * decompressed size. */
    if (usb_file_read(ode, reader, header, sizeof(header), 1) < 0)
        goto done;
    if (memcmp(header, "OM3DCOV2", 8) || header[8] || header[9] != width || header[10] ||
        header[11] != height || read_be32(header + 20) != pixel_bytes)
        goto done;
    compressed_bytes = read_be32(header + 12);
    /* zlib can make incompressible pixels slightly bigger than raw. */
    if (!compressed_bytes ||
        compressed_bytes > (width == USB_COVER_WIDTH ? 32768UL : pixel_bytes + 1024UL))
        goto done;

    /* The compressed data, its CRC, and the footer right after it. */
    compressed = (unsigned char *)malloc((size_t)compressed_bytes);
    decompressor = (tinfl_decompressor *)malloc(sizeof(*decompressor));
    check_start(&check, 0);
    if (!compressed || !decompressor ||
        read_checked(ode, reader, compressed, compressed_bytes, &check, cancel, context) < 0 ||
        !check_matches(&check, read_be32(header + 16)) ||
        usb_file_verify(ode, reader, (const unsigned char *)"END3DC02", 8) < 0)
        goto done;

    /* Decompress straight into the picture; it must fill it exactly. */
    in_bytes = (size_t)compressed_bytes;
    tinfl_init(decompressor);
    status =
        tinfl_decompress(decompressor, compressed, &in_bytes, pixels, pixels, &out_bytes,
                         TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (status == TINFL_STATUS_DONE && in_bytes == compressed_bytes && out_bytes == pixel_bytes)
        result = 0;

done:
    free(compressed);
    free(decompressor);
    if (usb_file_close(ode) < 0)
        return -1;
    return result;
}

int usb_fast_cover(Ode *ode, OdeReadBuffer reader, const char *filename, unsigned char *pixels,
                   MediaCancel cancel, void *context)
{
    return load_prepared_cover(ode, reader, "covers", filename, pixels, USB_COVER_WIDTH,
                               USB_COVER_HEIGHT, cancel, context);
}

int usb_large_cover(Ode *ode, OdeReadBuffer reader, const char *filename, unsigned char *pixels,
                    MediaCancel cancel, void *context)
{
    char name[104];
    size_t length = strlen(filename);

    /* "NAME.png" or "NAME.3dc" becomes "NAME-large.3dc". */
    if (length < 5 || length > 96 || filename[length - 4] != '.')
        return -1;
    memcpy(name, filename, length - 4);
    strcpy(name + length - 4, "-large.3dc");
    return load_prepared_cover(ode, reader, "covers", name, pixels, USB_LARGE_COVER_WIDTH,
                               USB_LARGE_COVER_HEIGHT, cancel, context);
}
