#include "preview_stream.h"
#include "usb_file.h"
#include <stdlib.h>
#include <string.h>
#ifdef OH3DO_SDK
#include "mem.h"
#include "kernel.h"
#include "task.h"
#include "io.h"
#include "item.h"
#include "nodes.h"
#include "kernelnodes.h"
#include "timerutils.h"
#include "ode_bus.h"
#endif

/* The reader runs above the menu's own task (150), so a finished transfer
 * is followed up at once. It spends nearly all its time waiting for the
 * drive: blocks land straight in a slot, are summed as they arrive, and a
 * chunk's entries are stored with plain copies. The bus moves each byte on
 * the CPU, though, so while the chunk being read is not needed for a while
 * the reader drops below the menu and reads only while the menu waits. */
#define READER_PRIORITY 160
#define READER_SPARE_PRIORITY 140
#define READER_SPARE_CHUNKS 2 /* ahead of playback by more than this */
/* Playback holds this many frames before a chunk that has not arrived, so
 * the sound pauses before it runs out and both resume in step. */
#define LOOKAHEAD 3
#define READER_STACK 8192
/* Sound is written this far ahead of the DSP at most; the rest of the ring
 * is what it may still be playing. */
#define RING_GUARD 2048UL
#define SLOT_SLACK 4UL /* the decoder reads up to two bytes past a packet */

static unsigned int be16(const unsigned char *p) { return ((unsigned int)p[0]<<8)|p[1]; }

/* Memory kinds: DRAM, VRAM, cel source, audio. Hosts use malloc. */
enum { MEM_DRAM, MEM_VRAM, MEM_CEL, MEM_AUDIO };
static void *memory(unsigned long bytes,int kind)
{
#ifdef OH3DO_SDK
    static const unsigned long types[]={MEMTYPE_DRAM,MEMTYPE_VRAM,MEMTYPE_CEL,MEMTYPE_AUDIO};
    return AllocMem((int32)bytes,types[kind]);
#else
    (void)kind;
    return malloc((size_t)bytes);
#endif
}
static void release(void *data,unsigned long bytes)
{
    if (!data) return;
#ifdef OH3DO_SDK
    FreeMem(data,(int32)bytes);
#else
    (void)bytes;
    free(data);
#endif
}

/* Sizes of the decoder's buffers, in the order they are allocated. */
#define MAP_BYTES (PREVIEW7_BLOCKS*2UL*sizeof(unsigned long))
#define DICT4_BYTES (PREVIEW7_MAX_ENTRIES4*32UL)
#define THUMB4_BYTES (PREVIEW7_MAX_ENTRIES4*8UL)
#define DICT2_BYTES (PREVIEW7_MAX_ENTRIES2*8UL)
#define THUMB2_BYTES (PREVIEW7_MAX_ENTRIES2*sizeof(unsigned long))

unsigned long preview_stream_bytes(const Preview7Header *header,unsigned int slots)
{
    return MAP_BYTES+DICT4_BYTES+THUMB4_BYTES+DICT2_BYTES+THUMB2_BYTES+PREVIEW7_THUMB_BYTES+
           slots*(header->max_chunk+SLOT_SLACK)+PREVIEW_STREAM_RING;
}

static void free_all(PreviewStream *s)
{
    unsigned int i;
    for (i=0;i<s->slots;++i) release(s->slot[i],s->slot_bytes);
    release(s->dec.map,MAP_BYTES); release(s->dec.dict4,DICT4_BYTES); release(s->dec.thumb4,THUMB4_BYTES);
    release(s->dec.dict2,DICT2_BYTES); release(s->dec.thumb2,THUMB2_BYTES);
    release(s->thumb,PREVIEW7_THUMB_BYTES); release(s->ring,PREVIEW_STREAM_RING);
}

/* DRAM first (or the given kind), then VRAM, then after reclaiming. */
static void *take(unsigned long bytes,int kind,PreviewStreamReclaim reclaim,void *context,unsigned char *in_vram)
{
    void *p=memory(bytes,kind);
    if (in_vram) *in_vram=0;
    if (!p && kind==MEM_DRAM) { p=memory(bytes,MEM_VRAM); if (p && in_vram) *in_vram=1; }
    if (!p && reclaim && reclaim(context)) p=memory(bytes,kind);
    return p;
}

/* ---- Reading ---- */

/* Read chunk sequence seq (counting across loops) into its slot, a block at
 * a time, summing it as it arrives. The file must be open at that chunk. */
static void pace(PreviewStream *s,unsigned long seq);
static int read_bytes(PreviewStream *s,OdeReadBuffer reader,unsigned long seq)
{
    unsigned int k=(unsigned int)(seq%s->slots),c=(unsigned int)(seq%s->header.chunks);
    unsigned long n=be16(s->header.table+2*c)*PREVIEW7_BLOCK_BYTES,at;
    Preview7Sum sum;
    sum.a=sum.b=0;
    s->reading=1;
    for (at=0;at<n;at+=PREVIEW7_BLOCK_BYTES) {
        pace(s,seq);
        if (usb_file_read_block(&s->ode,reader,s->slot[k]+at)<0) { s->reading=0; return -1; }
        /* The chunk's own check, its first word, is not summed. */
        preview7_sum_add(&sum,s->slot[k]+at+(at ? 0 : 4),PREVIEW7_BLOCK_BYTES-(at ? 0 : 4));
    }
    s->reading=0;
    s->slot_sum[k]=preview7_sum_value(&sum);
    return 0;
}
/* Check the chunk just read, store its entries and hand it to the player. */
static int finish_chunk(PreviewStream *s,unsigned long seq)
{
    unsigned int k=(unsigned int)(seq%s->slots),c=(unsigned int)(seq%s->header.chunks);
    if (preview7_chunk_summed(&s->header,c,s->slot[k],be16(s->header.table+2*c)*PREVIEW7_BLOCK_BYTES,s->slot_sum[k],&s->info[k])) return -1;
    preview7_define(&s->dec,&s->info[k]);
    ++s->chunks_read;
    s->ready[k]=seq+1;
    return 0;
}
static int read_chunk(PreviewStream *s,OdeReadBuffer reader,unsigned long seq)
{
    return read_bytes(s,reader,seq)<0 ? -1 : finish_chunk(s,seq);
}

/* Open the file and read past the header block, which must not have changed. */
static int open_file(PreviewStream *s,OdeReadBuffer reader)
{
    static unsigned char block[PREVIEW7_BLOCK_BYTES];
    if (usb_file_open(&s->ode,s->name)<0) return -1;
    if (usb_file_read(&s->ode,reader,block,PREVIEW7_BLOCK_BYTES,USB_FILE_CRC_CHECKED)<0 ||
        memcmp(block,s->block,PREVIEW7_BLOCK_BYTES)) { usb_file_close(&s->ode); return -1; }
    return 0;
}

/* Before chunk seq: start the file over at a new loop. */
static int position(PreviewStream *s,OdeReadBuffer reader,unsigned long seq)
{
    if (seq%s->header.chunks) return 0;
    if (seq && usb_file_close(&s->ode)<0) return -1;
    return open_file(s,reader);
}

#ifdef OH3DO_SDK
static PreviewStream *reader_stream;
static int reader_spare;
static void pace(PreviewStream *s,unsigned long seq)
{
    unsigned long need=(seq/s->header.chunks)*s->header.frames+(seq%s->header.chunks)*(unsigned long)s->header.chunk_frames;
    /* Loading before the first frame or during a wait is always urgent. */
    int spare=*(volatile int *)&s->playing && !*(volatile int *)&s->paused &&
        need>*(volatile unsigned long *)&s->frame+READER_SPARE_CHUNKS*(unsigned long)s->header.chunk_frames;
    if (spare!=reader_spare) { reader_spare=spare; SetItemPri(CURRENTTASK->t.n_Item,(uint8)(spare ? READER_SPARE_PRIORITY : READER_PRIORITY)); }
    if (spare) ++s->spare_blocks;
}
static void reader_main(void)
{
    PreviewStream *s=reader_stream;
    OdeBus bus;
    unsigned long seq=0;
    reader_spare=0;
    if (ode_bus_open(&bus)<0) { s->error=1; s->running=0; return; }
    bus.blocking=1;
    s->ode.transfer=ode_bus_transfer; s->ode.context=&bus; s->ode.progress=NULL;
    while (!s->stop) {
        unsigned long started,spent;
        if (seq>=s->released+s->slots) { SleepMSec(bus.timer,4); continue; }
        /* The next loop is read only once this one has played: the clip's
         * end plays without the reader competing, then the picture holds
         * while every slot fills (see decode). */
        if (seq && !(seq%s->header.chunks) &&
            *(volatile unsigned long *)&s->frame<(seq/s->header.chunks)*s->header.frames) { SleepMSec(bus.timer,4); continue; }
        started=(unsigned long)GetMSecTime(bus.timer);
        if (position(s,ode_bus_read_buffer,seq)<0 || read_bytes(s,ode_bus_read_buffer,seq)<0) {
            s->reader_error_code=(long)bus.last_error; s->error=2; break;
        }
        spent=(unsigned long)GetMSecTime(bus.timer)-started;
        s->read_ms+=spent; if (spent>s->read_max_ms) s->read_max_ms=spent;
        if (finish_chunk(s,seq)<0) { s->error=2; break; }
        ++seq;
    }
    if (!bus.timed_out) usb_file_close(&s->ode);
    s->bus_commands=bus.commands;
    ode_bus_close(&bus);
    s->running=0;
}
/* A thread that may use XBUS: CreateThread without the privilege it lacks
 * (the kernel's CREATETASK_TAG_SUPER is CREATETASK_TAG_PRIVATE0 in the
 * public headers). */
static long start_reader(PreviewStream *s)
{
    TagArg tags[8];
    unsigned char *stack=(unsigned char *)AllocMem(READER_STACK,MEMTYPE_ANY);
    Item item;
    if (!stack) return -1;
    reader_stream=s;
    tags[0].ta_Tag=TAG_ITEM_PRI; tags[0].ta_Arg=(void *)READER_PRIORITY;
    tags[1].ta_Tag=TAG_ITEM_NAME; tags[1].ta_Arg=(void *)"Oh My 3DO preview reader";
    tags[2].ta_Tag=CREATETASK_TAG_PC; tags[2].ta_Arg=(void *)reader_main;
    tags[3].ta_Tag=CREATETASK_TAG_STACKSIZE; tags[3].ta_Arg=(void *)READER_STACK;
    tags[4].ta_Tag=CREATETASK_TAG_SP; tags[4].ta_Arg=(void *)(stack+READER_STACK);
    tags[5].ta_Tag=CREATETASK_TAG_ALLOCDTHREADSP; tags[5].ta_Arg=0;
    tags[6].ta_Tag=CREATETASK_TAG_PRIVATE0; tags[6].ta_Arg=0;
    tags[7].ta_Tag=TAG_END;
    s->running=1;
    item=CreateItem(MKNODEID(KERNELNODE,TASKNODE),tags);
    if (item<0) { FreeMem(stack,READER_STACK); s->running=0; }
    return item;
}
static void sleep_ms(unsigned long ms)
{
    Item timer=GetTimerIOReq();
    if (timer>=0) { SleepMSec(timer,ms); DeleteIOReq(timer); }
}
#else
static void pace(PreviewStream *s,unsigned long seq) { (void)s; (void)seq; }
/* Hosts: read what the slots have room for, now. */
static OdeReadBuffer host_reader;
static void pump(PreviewStream *s)
{
    while (!s->error && s->next_read<s->released+s->slots) {
        if (position(s,host_reader,s->next_read)<0 || read_chunk(s,host_reader,s->next_read)<0) { s->error=2; return; }
        ++s->next_read;
    }
}
#endif

/* ---- Sound ---- */

static unsigned long samples_to(unsigned long frame) { return frame*PREVIEW7_AUDIO_RATE/PREVIEW7_FPS; }
/* Sound samples from the clip's first start to frame f, across loops. */
static unsigned long clip_samples(const PreviewStream *s,unsigned long f)
{
    return (f/s->header.frames)*samples_to(s->header.frames)+samples_to(f%s->header.frames);
}
static unsigned long sequence_frame(const PreviewStream *s,unsigned long seq)
{
    return (seq/s->header.chunks)*s->header.frames+(seq%s->header.chunks)*(unsigned long)s->header.chunk_frames;
}
/* ms * rate / 1000 without overflowing 32 bits after a few minutes. */
static unsigned long per_ms(unsigned long ms,unsigned long rate)
{
    return (ms/1000UL)*rate+(ms%1000UL)*rate/1000UL;
}
static unsigned long elapsed_ms(const PreviewStream *s,unsigned long now)
{
    unsigned long paused=s->audio_paused_ms+(s->paused ? now-s->pause_at : 0);
    return now-s->audio_since-paused;
}

/* Copy ready chunks' sound into the ring, in order, while there is room. */
static void fill(PreviewStream *s,unsigned long now)
{
    unsigned long played=per_ms(elapsed_ms(s,now),PREVIEW7_AUDIO_RATE);
    for (;;) {
        unsigned long seq=s->audio_written,k=seq%s->slots,at,n,i;
        const Preview7Chunk *info;
        if (seq>=s->released+s->slots || s->ready[k]!=seq+1) return;
        info=&s->info[k];
        n=info->audio_samples;
        at=clip_samples(s,sequence_frame(s,seq));
        if (at+n>played+PREVIEW_STREAM_RING-RING_GUARD) return;
        if (s->ring) for (i=0;i<n;++i) s->ring[(at+i)%PREVIEW_STREAM_RING]=info->audio[i];
        ++s->audio_written;
    }
}

/* Slots are free once their frames are decoded and their sound is in the ring. */
static void release_done(PreviewStream *s)
{
    for (;;) {
        unsigned long seq=s->released;
        unsigned int k=(unsigned int)(seq%s->slots);
        if (s->ready[k]!=seq+1 || sequence_frame(s,seq)+s->info[k].frames>s->frame) return;
        if (s->sound && seq>=s->audio_written) return;
        s->released=seq+1;
    }
}

void preview_stream_audio(PreviewStream *s,unsigned long now,int waiting)
{
    if (!s->playing) return;
    if (waiting && !s->paused) {
        s->paused=s->loop_hold ? 2 : 1; s->pause_at=now; preview_audio_pause(&s->audio,1);
        if (!s->loop_hold && s->stalls<4) {
            unsigned long *log=s->stall_log[s->stalls],slots=0;
            unsigned int k;
            for (k=0;k<s->slots && k<8;++k) slots|=(s->ready[k]&15UL)<<(4*k);
            log[0]=s->frame; log[1]=s->released; log[2]=slots; log[3]=elapsed_ms(s,now);
        }
        if (s->loop_hold) ++s->loop_holds;
        else ++s->stalls;
    }
    else if (!waiting && s->paused) {
        if (s->paused==2) s->loop_hold_ms+=now-s->pause_at; else s->stall_ms+=now-s->pause_at;
        s->paused=0; s->audio_paused_ms+=now-s->pause_at;
        preview_audio_pause(&s->audio,0);
    }
    if (!s->paused && s->sound) fill(s,now);
    release_done(s);
}

unsigned long preview_stream_due(PreviewStream *s,unsigned long now)
{
    return s->playing ? per_ms(elapsed_ms(s,now),PREVIEW7_FPS) : 0;
}

void preview_stream_play(PreviewStream *s,unsigned long now)
{
    if (!s->open || s->playing) return;
    s->audio_since=now; s->playing=1;
    if (s->sound) {
        fill(s,now);
        if (preview_audio_start_ring(&s->audio,(unsigned char *)s->ring,PREVIEW_STREAM_RING,PREVIEW7_AUDIO_RATE)<0) s->sound=0;
    }
}

/* ---- Playing ---- */

static int decode(PreviewStream *s,unsigned long frame,int picture,unsigned char *screen)
{
    int p=picture==PREVIEW7_TV;
    if (s->error) return -1;
    s->dec.screen=screen;
    if (!s->picture_valid[p]) { preview7_render(&s->dec,picture); s->picture_valid[p]=1; }
    if (s->frame<=frame) {
        /* Hold a little before a missing chunk (see LOOKAHEAD). */
        unsigned long ahead=frame+LOOKAHEAD,seq=(ahead/s->header.frames)*s->header.chunks+(ahead%s->header.frames)/s->header.chunk_frames;
#ifndef OH3DO_SDK
        pump(s);
#endif
        s->loop_hold=0;
        /* Not across a loop: the next loop's chunks wait for this one to end. */
        if (ahead/s->header.frames==frame/s->header.frames &&
            s->ready[seq%s->slots]!=seq+1 && seq<s->released+s->slots) return s->error ? -1 : PREVIEW_STREAM_NOT_READY;
        if (s->frame && !(s->frame%s->header.frames)) {
            /* Start each loop with every slot full, as the first start does,
             * so its opening chunks are not read while they play. */
            unsigned long first=(s->frame/s->header.frames)*s->header.chunks,n=s->slots<s->header.chunks ? s->slots : s->header.chunks,j;
#ifndef OH3DO_SDK
            pump(s);
#endif
            for (j=0;j<n;++j)
                if (s->ready[(first+j)%s->slots]!=first+j+1) { s->loop_hold=1; return s->error ? -1 : PREVIEW_STREAM_NOT_READY; }
        }
    }
    while (s->frame<=frame) {
        unsigned long local=s->frame%s->header.frames,seq=(s->frame/s->header.frames)*s->header.chunks+local/s->header.chunk_frames;
        unsigned int k=(unsigned int)(seq%s->slots);
#ifndef OH3DO_SDK
        pump(s);
#endif
        if (s->ready[k]!=seq+1) return s->error ? -1 : PREVIEW_STREAM_NOT_READY;
        if (!local && s->frame) {
            /* Each loop decodes from black, as the encoder did. */
            preview7_reset(&s->dec);
            preview7_render(&s->dec,picture);
            s->picture_valid[!p]=0;
        }
        if (preview7_frame(&s->dec,&s->info[k],(unsigned int)(local%s->header.chunk_frames),picture)) { s->error=3; return -1; }
        s->picture_valid[!p]=0;
        /* A finished chunk frees its slot for the reader at once. */
        if (++s->frame==sequence_frame(s,seq)+s->info[k].frames) release_done(s);
    }
    release_done(s);
    return 0;
}

int preview_stream_decode(PreviewStream *s,unsigned long frame,int picture,unsigned char *screen)
{
    return s->open ? decode(s,frame,picture,screen) : -1;
}

void preview_stream_invalidate(PreviewStream *s,int picture)
{
    s->picture_valid[picture==PREVIEW7_TV]=0;
}

void preview_stream_close(PreviewStream *s)
{
#ifdef OH3DO_SDK
    if (s->thread>0) {
        unsigned long waited=0;
        s->stop=1;
        while (s->running && waited<5000UL) { sleep_ms(5); waited+=5; }
        DeleteThread((Item)s->thread);
        if (s->running) s->error=4;
    }
#endif
    preview_audio_close(&s->audio);
    free_all(s);
    {
        PreviewStream cleared;
        memset(&cleared,0,sizeof(cleared));
        cleared.chunks_read=s->chunks_read; cleared.read_ms=s->read_ms; cleared.read_max_ms=s->read_max_ms;
        cleared.stalls=s->stalls; cleared.stall_ms=s->stall_ms; cleared.error=s->error;
        cleared.loop_holds=s->loop_holds; cleared.loop_hold_ms=s->loop_hold_ms; cleared.spare_blocks=s->spare_blocks;
        memcpy(cleared.stall_log,s->stall_log,sizeof(cleared.stall_log));
        cleared.reader_error_code=s->reader_error_code; cleared.bus_commands=s->bus_commands;
        cleared.slots=s->slots;
        cleared.decode_ms=s->decode_ms; cleared.decode_frames=s->decode_frames; cleared.idle_ms=s->idle_ms;
        cleared.idle_frames=s->idle_frames; cleared.full_ms=s->full_ms; cleared.full_frames=s->full_frames;
        *s=cleared;
    }
}

int preview_stream_open(PreviewStream *s,const Ode *ode,OdeReadBuffer reader,const char *game_folder,const char *name,
                        const unsigned char *header_block,int sound,PreviewStreamCancel cancel,void *cancel_context,
                        PreviewStreamReclaim reclaim,void *reclaim_context)
{
    unsigned int i,want,ready;
    int result=-1;
    memset(s,0,sizeof(*s));
    if (strlen(game_folder)>=sizeof(s->folder) || strlen(name)>=sizeof(s->name)) return -1;
    memcpy(s->block,header_block,PREVIEW7_BLOCK_BYTES);
    if (preview7_header(s->block,&s->header)) return -1;
    s->header.table=s->block+PREVIEW7_HEADER_BYTES;
    strcpy(s->folder,game_folder); strcpy(s->name,name);
    s->ode=*ode; s->ode.navigation_failed=0;
    s->ode.media_beside=1; s->ode.media_folder=s->folder[0] ? s->folder : NULL;
    s->sound=sound && s->header.audio_rate;
    /* The decoder first: it is used on every frame. */
    s->dec.map=(unsigned long *)take(MAP_BYTES,MEM_DRAM,reclaim,reclaim_context,0);
    s->dec.dict4=(unsigned char *)take(DICT4_BYTES,MEM_DRAM,reclaim,reclaim_context,0);
    s->dec.thumb4=(unsigned char *)take(THUMB4_BYTES,MEM_DRAM,reclaim,reclaim_context,0);
    s->dec.dict2=(unsigned char *)take(DICT2_BYTES,MEM_DRAM,reclaim,reclaim_context,0);
    s->dec.thumb2=(unsigned long *)take(THUMB2_BYTES,MEM_DRAM,reclaim,reclaim_context,0);
    s->thumb=(unsigned char *)take(PREVIEW7_THUMB_BYTES,MEM_CEL,reclaim,reclaim_context,0);
    if (s->sound) s->ring=(signed char *)take(PREVIEW_STREAM_RING,MEM_AUDIO,reclaim,reclaim_context,0);
    s->dec.thumb=s->thumb; s->dec.entries4=PREVIEW7_MAX_ENTRIES4; s->dec.entries2=PREVIEW7_MAX_ENTRIES2;
    if (!s->dec.map || !s->dec.dict4 || !s->dec.thumb4 || !s->dec.dict2 || !s->dec.thumb2 || !s->thumb || (s->sound && !s->ring)) {
        result=-4; goto failed;
    }
    /* Then as many chunk slots as the encoder assumed, video RAM first. */
    s->slot_bytes=s->header.max_chunk+SLOT_SLACK;
    want=s->header.slots<PREVIEW_STREAM_SLOTS ? s->header.slots : PREVIEW_STREAM_SLOTS;
    for (i=0;i<want;++i) {
        s->slot[i]=(unsigned char *)memory(s->slot_bytes,MEM_VRAM); s->slot_vram[i]=1;
        if (!s->slot[i]) { s->slot[i]=(unsigned char *)take(s->slot_bytes,MEM_DRAM,reclaim,reclaim_context,&s->slot_vram[i]); }
        if (!s->slot[i]) break;
        s->slot[i][s->header.max_chunk]=s->slot[i][s->header.max_chunk+1]=0;
        ++s->slots;
    }
    /* One slot playing while another is read is the least that streams;
     * fewer slots than the encoder assumed only means longer waits. */
    if (s->slots<2 && s->slots<s->header.chunks) { result=-4; goto failed; }
    if (s->ring) memset(s->ring,0,PREVIEW_STREAM_RING);
    preview7_reset(&s->dec);
#ifdef OH3DO_SDK
    (void)reader;
    s->thread=start_reader(s);
    if (s->thread<0) goto failed;
#else
    host_reader=reader;
#endif
    /* Wait for every slot (at least the clip's preroll): the first chunks
     * carry most of the dictionary, and checking them waits for the menu's
     * spare time, which is short just after a preview starts. */
    want=s->slots<s->header.chunks ? s->slots : s->header.chunks;
    for (;;) {
        for (ready=0;ready<want && s->ready[ready]==ready+1;++ready) {}
        if (ready==want) break;
        if (s->error) goto failed;
        if (cancel && cancel(cancel_context)) { result=-5; goto failed; }
#ifdef OH3DO_SDK
        sleep_ms(5);
#else
        pump(s);
#endif
    }
    if (decode(s,0,PREVIEW7_TV,NULL)) goto failed;
    s->open=1;
    return 0;
failed:
    preview_stream_close(s);
    return result;
}
