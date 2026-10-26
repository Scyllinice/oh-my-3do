/*
 * Portfolio helpers for the OM3DVID7 player. See om3dvid7_portfolio.h.
 */
#include "om3dvid7_portfolio.h"
#include "om3dvid7_player.h"
#include "mem.h"
#include "kernel.h"
#include "task.h"
#include "io.h"
#include "item.h"
#include "nodes.h"
#include "kernelnodes.h"
#include "timerutils.h"

void *om3dvid7_portfolio_allocate(void *context, unsigned long bytes, int kind)
{
    /* In the order of the OM3DVID7_MEMORY_* kinds. */
    static const unsigned long types[] = {MEMTYPE_DRAM, MEMTYPE_VRAM, MEMTYPE_CEL, MEMTYPE_AUDIO};

    (void)context;
    return AllocMem((int32)bytes, types[kind]);
}

void om3dvid7_portfolio_release(void *context, void *memory, unsigned long bytes)
{
    (void)context;
    FreeMem(memory, (int32)bytes); /* Portfolio needs the allocated size */
}

/*
 * The task item is created directly so that a privileged thread can be
 * asked for. (The kernel's CREATETASK_TAG_SUPER is CREATETASK_TAG_PRIVATE0
 * in the public headers.)
 */
long om3dvid7_portfolio_start_thread(void (*entry)(void), const char *name, int priority,
                                     unsigned long stack_bytes, int privileged)
{
    TagArg tags[8];
    unsigned char *stack = (unsigned char *)AllocMem((int32)stack_bytes, MEMTYPE_ANY);
    int count = 0;
    Item item;

    if (!stack)
        return -1;
    tags[count].ta_Tag = TAG_ITEM_PRI;
    tags[count++].ta_Arg = (void *)priority;
    tags[count].ta_Tag = TAG_ITEM_NAME;
    tags[count++].ta_Arg = (void *)name;
    tags[count].ta_Tag = CREATETASK_TAG_PC;
    tags[count++].ta_Arg = (void *)entry;
    tags[count].ta_Tag = CREATETASK_TAG_STACKSIZE;
    tags[count++].ta_Arg = (void *)stack_bytes;
    tags[count].ta_Tag = CREATETASK_TAG_SP;
    tags[count++].ta_Arg = (void *)(stack + stack_bytes); /* stacks grow downward */
    tags[count].ta_Tag = CREATETASK_TAG_ALLOCDTHREADSP;
    tags[count++].ta_Arg = 0;
    if (privileged) {
        tags[count].ta_Tag = CREATETASK_TAG_PRIVATE0;
        tags[count++].ta_Arg = 0;
    }
    tags[count].ta_Tag = TAG_END;

    item = CreateItem(MKNODEID(KERNELNODE, TASKNODE), tags);
    if (item < 0)
        FreeMem(stack, (int32)stack_bytes);
    return item;
}

void om3dvid7_portfolio_sleep(unsigned long ms)
{
    Item timer = GetTimerIOReq();
    if (timer >= 0) {
        SleepMSec(timer, ms);
        DeleteIOReq(timer);
    }
}
