#ifndef _SYNETSTATE_H_
#define _SYNETSTATE_H_

#include <PR/ultratypes.h>
#include <ssb_types.h>

/*
 * sys/netstate.c — simulation snapshot/restore and the rollback driver.
 *
 * A snapshot S(t) is the whole VS simulation as it stands right before tick t
 * is simulated (netinput tick == t): the used part of the scene arena
 * (gPortSceneHeap, every GObj/fighter/item/weapon/stage object), the decomp
 * globals that port/netstate_section.h groups into the state section, the
 * suspended GObj-thread coroutines (stack + registers), and netinput's
 * per-tick bookkeeping. Restoring S(t) and re-running task_update once per
 * tick with the inputs now known for t.. reproduces the present with
 * corrected remote inputs; that re-run is the "resimulation".
 *
 * syNetStateRunTick() is called by syTaskmanRunTask() in place of
 * tfunc->task_update(tfunc) and does, in order: save S(cur), honour a pending
 * rollback request (restore + resimulate up to cur), run the SyncTest if
 * enabled, then simulate cur normally.
 *
 * Debug environment:
 *   SSB64_ROLLBACK=1             enable snapshots + rollback in VS battle
 *                                (implied by SSB64_NETPLAY=1 and by SyncTest)
 *   SSB64_ROLLBACK_SLOTS=N       snapshot ring depth (default 10 = max rollback)
 *   SSB64_ROLLBACK_SYNCTEST=N    every tick, roll back N ticks and resimulate;
 *                                log any gameplay-hash column that differs
 */

#define SYNETSTATE_SLOTS_DEFAULT 10
#define SYNETSTATE_SLOTS_MAX 60

extern void syNetStateInitDebugEnv(void);
extern void syNetStateSetEnabled(sb32 enabled);
extern void syNetStateStartVSSession(void);
extern void syNetStateStopVSSession(void);

extern sb32 syNetStateCheckActive(void);
extern sb32 syNetStateCheckResimulating(void);
extern void syNetStateGetStats(u32 *rollbacks, u32 *resim_ticks, u32 *max_depth, u32 *stalls, u32 *timesync_skips);
extern u32 syNetStateGetOldestTick(void);
extern u32 syNetStateGetRollbackSlots(void);

extern sb32 syNetStateSave(u32 tick);
extern sb32 syNetStateRestore(u32 tick);

/* Called by netinput when a confirmed remote input contradicts the prediction
 * published for an already-simulated tick. Requests are merged to the oldest
 * tick and honoured at the next syNetStateRunTick(). */
extern void syNetStateRequestRollback(u32 tick);
extern sb32 syNetStateCheckRollbackPending(void);

/* Taskman hook: runs exactly one real tick, plus whatever rollback /
 * resimulation work is pending before it. */
extern void syNetStateRunTick(void (*task_update)(void *), void *tfunc);

#endif /* _SYNETSTATE_H_ */
