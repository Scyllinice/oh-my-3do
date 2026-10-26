#ifndef OM3DVID7_PORTFOLIO_H
#define OM3DVID7_PORTFOLIO_H
/*
 * Helpers for hosting the OM3DVID7 player (om3dvid7_player.h) on the 3DO's
 * operating system, Portfolio. Console builds only.
 *
 * The memory pair fits Om3dvid7PlayerHost.allocate and .release directly.
 * A host that wants a reader thread starts it with
 * om3dvid7_portfolio_start_thread from its start_reader callback; the
 * thread's own function opens whatever it reads from, then calls
 * om3dvid7_player_read in a loop (sleeping a few milliseconds whenever it
 * answers OM3DVID7_PLAYER_READ_LATER).
 */

/* Memory of an OM3DVID7_MEMORY_* kind from AllocMem, and freeing it. */
void *om3dvid7_portfolio_allocate(void *context, unsigned long bytes, int kind);
void om3dvid7_portfolio_release(void *context, void *memory, unsigned long bytes);

/*
 * Starts `entry` as a thread of the current task, at `priority`, with a
 * stack of `stack_bytes` that the kernel frees when the thread is deleted.
 * `privileged` 1 lets the thread use the XBUS directly (which CreateThread
 * cannot grant); the program must itself be privileged. Returns the
 * thread's item, or a negative error.
 */
long om3dvid7_portfolio_start_thread(void (*entry)(void), const char *name, int priority,
                                     unsigned long stack_bytes, int privileged);

/* Sleeps the calling task or thread for `ms` milliseconds. */
void om3dvid7_portfolio_sleep(unsigned long ms);
#endif
