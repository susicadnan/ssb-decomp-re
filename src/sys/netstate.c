#include <sys/netstate.h>

#include <sys/netinput.h>
#include <sys/netpeer.h>
#include <sys/netsync.h>
#include <sys/taskman.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <coroutine.h>
#include <netstate_coro.h>
#include <netstate_host.h>

/*
 * Simulation snapshot/restore and the rollback driver. See netstate.h for
 * the model and docs/rollback_design.md for the reasoning.
 *
 * This TU is excluded from the state section on purpose: everything here is
 * rollback bookkeeping and must survive a restore.
 */

extern void port_log(const char *fmt, ...);
/* The decomp stdlib.h shadow has no atoi; netpeer.c declares it the same way. */
extern int atoi(const char *s);
/* port/port_watchdog.cpp: tick liveness for the hang report. */
extern void port_watchdog_note_tick(void);

/* Where the driver is right now, readable from the watchdog thread. */
enum
{
	nSYNetStatePhaseIdle,
	nSYNetStatePhaseRollbackResim,
	nSYNetStatePhaseSyncTestResim,
	nSYNetStatePhaseLiveTick,
	nSYNetStatePhaseStall,
	nSYNetStatePhaseTimeSync,
};
static volatile u32 sSYNetStateDiagPhase;
static volatile u32 sSYNetStateDiagTick;
static volatile u32 sSYNetStateDiagResimTick;

/* taskman.c (PORT): the one malloc'd block every scene arena lives in. */
extern void *gPortSceneHeap;
extern const size_t gPortSceneHeapSize;

/* port/stubs/netstate_section.c */
extern int syNetStateSectionGetRanges(void **starts, size_t *sizes, int max_ranges);

#define SYNETSTATE_RANGES_MAX 4
#define SYNETSTATE_LOG_LIMIT 32
#define SYNETSTATE_DISCONNECT_FRAMES 600

typedef struct SYNetStateSlot
{
	sb32 is_valid;
	u32 tick;

	u8 *arena;
	size_t arena_capacity;
	size_t arena_used;

	u8 *globals;

	s32 coro_count;
	PortGObjCoroutineRecord coro_recs[PORT_GOBJ_COROUTINE_MAX];
	size_t coro_offsets[PORT_GOBJ_COROUTINE_MAX];
	u8 *coro_blobs;
	size_t coro_blob_capacity;
	size_t coro_blob_used;

	SYNetInputState input;

	/* Port-side bookkeeping about the arena bytes (byte-order fixups, reloc
	 * file registry, token table); see port/netstate_host.h. */
	void *host;

} SYNetStateSlot;

static SYNetStateSlot sSYNetStateSlots[SYNETSTATE_SLOTS_MAX];
static u32 sSYNetStateSlotCount = SYNETSTATE_SLOTS_DEFAULT;

static sb32 sSYNetStateIsEnabled;
static sb32 sSYNetStateIsActive;
static sb32 sSYNetStateIsResimulating;
static u32 sSYNetStateSyncTestDepth;

static sb32 sSYNetStateIsRollbackPending;
static u32 sSYNetStateRollbackTick;

static void *sSYNetStateRangeStarts[SYNETSTATE_RANGES_MAX];
static size_t sSYNetStateRangeSizes[SYNETSTATE_RANGES_MAX];
static s32 sSYNetStateRangeCount;
static size_t sSYNetStateGlobalsBytes;

/* Session statistics, logged at stop. */
static u32 sSYNetStateSaveCount;
static u32 sSYNetStateRollbackCount;
static u32 sSYNetStateResimTickCount;
static u32 sSYNetStateMaxRollbackDepth;
static u32 sSYNetStateRollbackTooOldCount;
static u32 sSYNetStateSyncTestRuns;
static u32 sSYNetStateSyncTestMismatches;
static u32 sSYNetStateStallCount;
static u32 sSYNetStateStallRun;       /* consecutive stalled frames */
static u32 sSYNetStateTimeSyncSkips;
static u32 sSYNetStateTimeSyncCooldown;
static u32 sSYNetStateLogCount;
static size_t sSYNetStateMaxArenaUsed;

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------

static sb32 syNetStateLogAllow(void)
{
	if (sSYNetStateLogCount >= SYNETSTATE_LOG_LIMIT)
	{
		return FALSE;
	}
	sSYNetStateLogCount++;

	if (sSYNetStateLogCount == SYNETSTATE_LOG_LIMIT)
	{
		port_log("SSB64 NetState: log limit reached, further messages suppressed until session end\n");
	}
	return TRUE;
}

/* Rate-limited logging: a desync can fire every tick, keep the log readable. */
#define syNetStateLog(...)                                                                                             \
	do                                                                                                                 \
	{                                                                                                                  \
		if (syNetStateLogAllow() != FALSE)                                                                             \
		{                                                                                                              \
			port_log(__VA_ARGS__);                                                                                     \
		}                                                                                                              \
	} while (0)

static u8 *syNetStateGrow(u8 *buf, size_t *capacity, size_t needed)
{
	u8 *next;
	size_t cap;

	if (needed <= *capacity)
	{
		return buf;
	}
	/* Grow in 256 KB steps so a slowly growing arena does not realloc every tick. */
	cap = (needed + 0x3FFFF) & ~(size_t)0x3FFFF;
	next = (u8 *)realloc(buf, cap);

	if (next == NULL)
	{
		syNetStateLog("SSB64 NetState: out of memory growing snapshot buffer to %zu bytes\n", cap);
		return buf;
	}
	*capacity = cap;
	return next;
}

static SYNetStateSlot *syNetStateFindSlot(u32 tick)
{
	SYNetStateSlot *slot = &sSYNetStateSlots[tick % sSYNetStateSlotCount];

	if ((slot->is_valid != FALSE) && (slot->tick == tick))
	{
		return slot;
	}
	return NULL;
}

static void syNetStateInvalidateSlots(void)
{
	u32 i;

	for (i = 0; i < SYNETSTATE_SLOTS_MAX; i++)
	{
		sSYNetStateSlots[i].is_valid = FALSE;
	}
}

static void syNetStateFreeSlots(void)
{
	u32 i;

	for (i = 0; i < SYNETSTATE_SLOTS_MAX; i++)
	{
		SYNetStateSlot *slot = &sSYNetStateSlots[i];

		free(slot->arena);
		free(slot->globals);
		free(slot->coro_blobs);
		port_netstate_host_free(slot->host);
		memset(slot, 0, sizeof(*slot));
	}
}

static size_t syNetStateGetArenaUsed(void)
{
	if ((gPortSceneHeap == NULL) || (gSYTaskmanGeneralHeap.ptr == NULL))
	{
		return 0;
	}
	if (((uintptr_t)gSYTaskmanGeneralHeap.ptr < (uintptr_t)gPortSceneHeap)
	    || ((uintptr_t)gSYTaskmanGeneralHeap.ptr > (uintptr_t)gPortSceneHeap + gPortSceneHeapSize))
	{
		/* The general heap is not the scene arena (should not happen in VS). */
		return 0;
	}
	return (size_t)((uintptr_t)gSYTaskmanGeneralHeap.ptr - (uintptr_t)gPortSceneHeap);
}

static u32 syNetStateCoroTickFn(void)
{
	return syNetInputGetTick();
}

// ----------------------------------------------------------------------------
// Debug environment / session lifecycle
// ----------------------------------------------------------------------------

void syNetStateInitDebugEnv(void)
{
#ifdef PORT
	const char *env;

	sSYNetStateIsEnabled = FALSE;
	sSYNetStateSyncTestDepth = 0;
	sSYNetStateSlotCount = SYNETSTATE_SLOTS_DEFAULT;

	env = getenv("SSB64_ROLLBACK");

	if ((env != NULL) && (atoi(env) != 0))
	{
		sSYNetStateIsEnabled = TRUE;
	}
	env = getenv("SSB64_NETPLAY");

	if ((env != NULL) && (atoi(env) != 0))
	{
		sSYNetStateIsEnabled = TRUE;
	}
	env = getenv("SSB64_ROLLBACK_SYNCTEST");

	if ((env != NULL) && (atoi(env) > 0))
	{
		sSYNetStateSyncTestDepth = (u32)atoi(env);
		sSYNetStateIsEnabled = TRUE;
	}
	env = getenv("SSB64_ROLLBACK_SLOTS");

	if ((env != NULL) && (atoi(env) > 0))
	{
		sSYNetStateSlotCount = (u32)atoi(env);
	}
	/* The ring must hold S(cur - depth) .. S(cur) for the SyncTest, plus one. */
	if (sSYNetStateSlotCount < sSYNetStateSyncTestDepth + 2)
	{
		sSYNetStateSlotCount = sSYNetStateSyncTestDepth + 2;
	}
	if (sSYNetStateSlotCount > SYNETSTATE_SLOTS_MAX)
	{
		sSYNetStateSlotCount = SYNETSTATE_SLOTS_MAX;
	}
	if (sSYNetStateIsEnabled != FALSE)
	{
		port_log("SSB64 NetState: rollback enabled slots=%u synctest=%u\n", sSYNetStateSlotCount,
		         sSYNetStateSyncTestDepth);
	}
#endif
}

/* A lobby-made netplay session (sys/netpeer.c) needs rollback regardless of
 * the debug environment. */
void syNetStateSetEnabled(sb32 enabled)
{
	sSYNetStateIsEnabled = enabled;

	if ((enabled != FALSE) && (sSYNetStateSlotCount < sSYNetStateSyncTestDepth + 2))
	{
		sSYNetStateSlotCount = sSYNetStateSyncTestDepth + 2;
	}
}

void syNetStateStartVSSession(void)
{
	size_t total;
	s32 i;

	syNetStateStopVSSession();

	if (sSYNetStateIsEnabled == FALSE)
	{
		return;
	}
	if (port_coroutine_snapshot_supported() == 0)
	{
		syNetStateLog("SSB64 NetState: coroutine backend cannot be snapshotted (Win32 fibers) — rollback disabled\n");
		return;
	}
	sSYNetStateRangeCount = syNetStateSectionGetRanges(sSYNetStateRangeStarts, sSYNetStateRangeSizes,
	                                                   SYNETSTATE_RANGES_MAX);
	total = 0;

	for (i = 0; i < sSYNetStateRangeCount; i++)
	{
		total += sSYNetStateRangeSizes[i];
	}
	if ((sSYNetStateRangeCount == 0) || (total == 0))
	{
		syNetStateLog("SSB64 NetState: state section is empty — build without PORT_ROLLBACK_STATE_SECTION? rollback disabled\n");
		return;
	}
	if (syNetStateGetArenaUsed() == 0)
	{
		syNetStateLog("SSB64 NetState: scene arena not set up — rollback disabled\n");
		return;
	}
	sSYNetStateGlobalsBytes = total;

	for (i = 0; i < (s32)sSYNetStateSlotCount; i++)
	{
		SYNetStateSlot *slot = &sSYNetStateSlots[i];

		slot->globals = (u8 *)malloc(sSYNetStateGlobalsBytes);

		if (slot->globals == NULL)
		{
			syNetStateLog("SSB64 NetState: out of memory for globals snapshots — rollback disabled\n");
			syNetStateFreeSlots();
			return;
		}
	}
	syNetStateInvalidateSlots();

	sSYNetStateIsActive = TRUE;
	sSYNetStateIsResimulating = FALSE;
	sSYNetStateIsRollbackPending = FALSE;
	sSYNetStateSaveCount = 0;
	sSYNetStateRollbackCount = 0;
	sSYNetStateResimTickCount = 0;
	sSYNetStateMaxRollbackDepth = 0;
	sSYNetStateRollbackTooOldCount = 0;
	sSYNetStateSyncTestRuns = 0;
	sSYNetStateSyncTestMismatches = 0;
	sSYNetStateStallCount = 0;
	sSYNetStateStallRun = 0;
	sSYNetStateTimeSyncSkips = 0;
	sSYNetStateTimeSyncCooldown = 0;
	sSYNetStateLogCount = 0;
	sSYNetStateMaxArenaUsed = 0;

	port_gobj_coroutine_set_rollback_active(1, syNetStateCoroTickFn);
	port_netstate_host_begin(gPortSceneHeap, gPortSceneHeapSize);

	syNetStateLog("SSB64 NetState: session start slots=%u globals=%zu bytes (%d ranges) arena_used=%zu synctest=%u\n",
	              sSYNetStateSlotCount, sSYNetStateGlobalsBytes, sSYNetStateRangeCount, syNetStateGetArenaUsed(),
	              sSYNetStateSyncTestDepth);
}

void syNetStateStopVSSession(void)
{
	if (sSYNetStateIsActive == FALSE)
	{
		return;
	}
	sSYNetStateIsActive = FALSE;
	sSYNetStateIsResimulating = FALSE;
	syNetInputSetResimulating(FALSE);

	port_gobj_coroutine_set_rollback_active(0, NULL);
	port_netstate_host_end();

	sSYNetStateLogCount = 0;
	syNetStateLog("SSB64 NetState: session end saves=%u rollbacks=%u resim_ticks=%u max_depth=%u too_old=%u "
	              "stalls=%u timesync_skips=%u max_arena_used=%zu synctest_runs=%u synctest_mismatches=%u\n",
	              sSYNetStateSaveCount, sSYNetStateRollbackCount, sSYNetStateResimTickCount,
	              sSYNetStateMaxRollbackDepth, sSYNetStateRollbackTooOldCount, sSYNetStateStallCount,
	              sSYNetStateTimeSyncSkips, sSYNetStateMaxArenaUsed, sSYNetStateSyncTestRuns,
	              sSYNetStateSyncTestMismatches);

	syNetStateFreeSlots();
}

sb32 syNetStateCheckActive(void)
{
	return sSYNetStateIsActive;
}

sb32 syNetStateCheckResimulating(void)
{
	return sSYNetStateIsResimulating;
}

u32 syNetStateGetRollbackSlots(void)
{
	return sSYNetStateSlotCount;
}

/* HUD (port/netplay/overlay.cpp). */
void syNetStateGetStats(u32 *rollbacks, u32 *resim_ticks, u32 *max_depth, u32 *stalls, u32 *timesync_skips)
{
	*rollbacks = sSYNetStateRollbackCount;
	*resim_ticks = sSYNetStateResimTickCount;
	*max_depth = sSYNetStateMaxRollbackDepth;
	*stalls = sSYNetStateStallCount;
	*timesync_skips = sSYNetStateTimeSyncSkips;
}

void syNetStateGetDiag(u32 *phase, u32 *tick, u32 *resim_tick, u32 *rollbacks, u32 *synctest_runs)
{
	*phase = sSYNetStateDiagPhase;
	*tick = sSYNetStateDiagTick;
	*resim_tick = sSYNetStateDiagResimTick;
	*rollbacks = sSYNetStateRollbackCount;
	*synctest_runs = sSYNetStateSyncTestRuns;
}

u32 syNetStateGetOldestTick(void)
{
	u32 oldest = 0xFFFFFFFFU;
	u32 i;

	for (i = 0; i < sSYNetStateSlotCount; i++)
	{
		if ((sSYNetStateSlots[i].is_valid != FALSE) && (sSYNetStateSlots[i].tick < oldest))
		{
			oldest = sSYNetStateSlots[i].tick;
		}
	}
	return oldest;
}

// ----------------------------------------------------------------------------
// Snapshot / restore
// ----------------------------------------------------------------------------

sb32 syNetStateSave(u32 tick)
{
	SYNetStateSlot *slot;
	size_t arena_used;
	size_t offset;
	s32 count;
	s32 i;

	if (sSYNetStateIsActive == FALSE)
	{
		return FALSE;
	}
	slot = &sSYNetStateSlots[tick % sSYNetStateSlotCount];
	slot->is_valid = FALSE;
	port_netstate_host_free(slot->host);
	slot->host = NULL;

	/* Arena: only the bump-allocated prefix is live. */
	arena_used = syNetStateGetArenaUsed();
	slot->arena = syNetStateGrow(slot->arena, &slot->arena_capacity, arena_used);

	if (slot->arena_capacity < arena_used)
	{
		return FALSE;
	}
	memcpy(slot->arena, gPortSceneHeap, arena_used);
	slot->arena_used = arena_used;

	if (arena_used > sSYNetStateMaxArenaUsed)
	{
		sSYNetStateMaxArenaUsed = arena_used;
	}
	/* Globals: the grouped state section(s), packed back to back. */
	offset = 0;

	for (i = 0; i < sSYNetStateRangeCount; i++)
	{
		memcpy(slot->globals + offset, sSYNetStateRangeStarts[i], sSYNetStateRangeSizes[i]);
		offset += sSYNetStateRangeSizes[i];
	}
	/* GObj-thread coroutines: registry plus a stack/register blob per live one. */
	count = port_gobj_coroutine_count();

	if (count > PORT_GOBJ_COROUTINE_MAX)
	{
		count = PORT_GOBJ_COROUTINE_MAX;
	}
	slot->coro_count = count;
	slot->coro_blob_used = 0;

	for (i = 0; i < count; i++)
	{
		const PortGObjCoroutineRecord *rec = port_gobj_coroutine_get(i);
		size_t blob_size;

		slot->coro_recs[i] = *rec;
		slot->coro_offsets[i] = slot->coro_blob_used;

		if ((rec->alive == 0) || (rec->co == NULL))
		{
			continue;
		}
		blob_size = port_coroutine_snapshot_size(rec->co);
		slot->coro_blobs = syNetStateGrow(slot->coro_blobs, &slot->coro_blob_capacity,
		                                  slot->coro_blob_used + blob_size);

		if (slot->coro_blob_capacity < slot->coro_blob_used + blob_size)
		{
			return FALSE;
		}
		port_coroutine_snapshot(rec->co, slot->coro_blobs + slot->coro_blob_used);
		slot->coro_blob_used += blob_size;
	}
	syNetInputSaveState(&slot->input);
	slot->host = port_netstate_host_save();

	slot->tick = tick;
	slot->is_valid = TRUE;
	sSYNetStateSaveCount++;

	/* Parked coroutines no snapshot in the ring can still name are garbage. */
	if (tick + 1 >= sSYNetStateSlotCount)
	{
		port_gobj_coroutine_collect(tick + 1 - sSYNetStateSlotCount);
	}
	/* Likewise, host-side journal history older than the oldest snapshot. */
	{
		SYNetStateSlot *oldest = syNetStateFindSlot(syNetStateGetOldestTick());

		if (oldest != NULL)
		{
			port_netstate_host_trim(oldest->host);
		}
	}
	return TRUE;
}

sb32 syNetStateRestore(u32 tick)
{
	SYNetStateSlot *slot;
	size_t arena_used_now;
	size_t offset;
	s32 i;

	if (sSYNetStateIsActive == FALSE)
	{
		return FALSE;
	}
	slot = syNetStateFindSlot(tick);

	if (slot == NULL)
	{
		return FALSE;
	}
	/* Measure before the globals restore moves the bump pointer back. */
	arena_used_now = syNetStateGetArenaUsed();

	/* Port bookkeeping about the arena bytes must agree with the bytes we
	 * are about to put back; it is independent of them, so order does not
	 * matter, but a failure here means the restore is incomplete. */
	if (port_netstate_host_restore(slot->host) == 0)
	{
		syNetStateLog("SSB64 NetState: host-side state for tick %u is gone — restore incomplete\n", tick);
	}
	memcpy(gPortSceneHeap, slot->arena, slot->arena_used);

	/* The arena was zeroed at scene start and the bump allocator never
	 * clears, so code may rely on fresh allocations being zero: scrub what
	 * the abandoned future allocated past the restored high-water mark. */
	if (arena_used_now > slot->arena_used)
	{
		memset((u8 *)gPortSceneHeap + slot->arena_used, 0, arena_used_now - slot->arena_used);
	}
	offset = 0;

	for (i = 0; i < sSYNetStateRangeCount; i++)
	{
		memcpy(sSYNetStateRangeStarts[i], slot->globals + offset, sSYNetStateRangeSizes[i]);
		offset += sSYNetStateRangeSizes[i];
	}
	port_gobj_coroutine_restore_set(slot->coro_recs, slot->coro_count);

	for (i = 0; i < slot->coro_count; i++)
	{
		const PortGObjCoroutineRecord *rec = &slot->coro_recs[i];

		if ((rec->alive == 0) || (rec->co == NULL))
		{
			continue;
		}
		/* restore_set has just verified the registry still holds this exact
		 * coroutine (address and serial); only then is its blob written. */
		if (port_gobj_coroutine_check(rec->co, rec->serial) == 0)
		{
			continue;
		}
		port_coroutine_restore(rec->co, slot->coro_blobs + slot->coro_offsets[i]);
	}
	syNetInputRestoreState(&slot->input);

	return TRUE;
}

// ----------------------------------------------------------------------------
// Rollback driver
// ----------------------------------------------------------------------------

void syNetStateRequestRollback(u32 tick)
{
	if ((sSYNetStateIsActive == FALSE) || (sSYNetStateIsResimulating != FALSE))
	{
		return;
	}
	if ((sSYNetStateIsRollbackPending == FALSE) || (tick < sSYNetStateRollbackTick))
	{
		sSYNetStateRollbackTick = tick;
	}
	sSYNetStateIsRollbackPending = TRUE;
}

sb32 syNetStateCheckRollbackPending(void)
{
	return sSYNetStateIsRollbackPending;
}

/* Restore S(from) and re-simulate ticks from..to-1, leaving the state at S(to)
 * and the ring holding every intermediate snapshot. */
static sb32 syNetStateResimulate(u32 from, u32 to, void (*task_update)(void *), void *tfunc)
{
	u32 tick;

	if (from >= to)
	{
		return TRUE;
	}
	if (syNetStateRestore(from) == FALSE)
	{
		sSYNetStateRollbackTooOldCount++;
		syNetStateLog("SSB64 NetState: cannot roll back to tick %u (oldest %u, current %u) — desync likely\n", from,
		              syNetStateGetOldestTick(), to);
		return FALSE;
	}
	sSYNetStateIsResimulating = TRUE;
	syNetInputSetResimulating(TRUE);

	for (tick = from; tick < to; tick++)
	{
		sSYNetStateDiagResimTick = tick;
		task_update(tfunc);
		sSYNetStateResimTickCount++;

		if (syNetInputGetTick() != tick + 1)
		{
			syNetStateLog("SSB64 NetState: tick did not advance during resimulation (expected %u got %u)\n", tick + 1,
			              syNetInputGetTick());
			break;
		}
		syNetStateSave(tick + 1);
	}
	sSYNetStateIsResimulating = FALSE;
	syNetInputSetResimulating(FALSE);

	if (to - from > sSYNetStateMaxRollbackDepth)
	{
		sSYNetStateMaxRollbackDepth = to - from;
	}
	return TRUE;
}

static void syNetStateRunSyncTest(u32 cur, void (*task_update)(void *), void *tfunc)
{
	SYNetSyncTickHash before;
	SYNetSyncTickHash after;
	u32 from = cur - sSYNetStateSyncTestDepth;
	s32 column;
	sb32 mismatch = FALSE;

	syNetSyncHashTick(&before);

	if (syNetStateResimulate(from, cur, task_update, tfunc) == FALSE)
	{
		return;
	}
	sSYNetStateSyncTestRuns++;
	syNetSyncHashTick(&after);

	for (column = 0; column < SYNETSYNC_COLUMN_NUM; column++)
	{
		if (before.column[column] != after.column[column])
		{
			if (mismatch == FALSE)
			{
				mismatch = TRUE;
				sSYNetStateSyncTestMismatches++;
				syNetStateLog("SSB64 NetState: SYNCTEST MISMATCH tick=%u depth=%u\n", cur, sSYNetStateSyncTestDepth);
			}
			syNetStateLog("SSB64 NetState:   column %-8s live=%08X resim=%08X\n", syNetSyncGetColumnName(column),
			              before.column[column], after.column[column]);
		}
	}
}

void syNetStateRunTick(void (*task_update)(void *), void *tfunc)
{
	u32 cur;

	if (sSYNetStateIsActive == FALSE)
	{
		task_update(tfunc);
		port_watchdog_note_tick();
		return;
	}
	cur = syNetInputGetTick();
	sSYNetStateDiagTick = cur;

	/* Stall rather than outrun the ring: a misprediction can be as old as
	 * the newest unconfirmed remote tick, and S(that tick) must still be
	 * here next tick. Keep the transport moving meanwhile; their inputs are
	 * what ends the stall and ours are what they wait for. */
	if (syNetInputGetRemoteLag() + 2 > sSYNetStateSlotCount)
	{
		sSYNetStateDiagPhase = nSYNetStatePhaseStall;
		sSYNetStateStallCount++;
		sSYNetStateStallRun++;

		if ((sSYNetStateStallCount % 60) == 1)
		{
			syNetStateLog("SSB64 NetState: stalling at tick %u, remote lag %u >= slots %u (stalls=%u)\n", cur,
			              syNetInputGetRemoteLag(), sSYNetStateSlotCount, sSYNetStateStallCount);
		}
		/* Ten seconds without a single input from the peer: they are gone.
		 * netpeer ends the match and sends everyone back to the menu. */
		if (sSYNetStateStallRun >= SYNETSTATE_DISCONNECT_FRAMES)
		{
			port_log("SSB64 NetState: no input from the peer for %u frames at tick %u — disconnecting\n",
			         sSYNetStateStallRun, cur);
			sSYNetStateStallRun = 0;
			syNetPeerAbortMatch("connection lost");
			return;
		}
		syNetPeerPumpWhileStalled();
		return;
	}
	sSYNetStateStallRun = 0;
	/* Time sync: when we are further ahead of the peer than they are of us
	 * by more than a frame, give one tick back now and then (at most one
	 * per 20) so both machines run in step and predictions stay shallow,
	 * instead of racing to the hard stall above. */
	if (sSYNetStateTimeSyncCooldown != 0)
	{
		sSYNetStateTimeSyncCooldown--;
	}
	else if (syNetPeerGetAdvantageDelta() > 1)
	{
		sSYNetStateDiagPhase = nSYNetStatePhaseTimeSync;
		sSYNetStateTimeSyncSkips++;
		sSYNetStateTimeSyncCooldown = 20;
		syNetPeerPumpWhileStalled();
		return;
	}
	/* S(cur): the present, before this tick runs. Re-saved below if a
	 * rollback rewrites the present. */
	syNetStateSave(cur);

	if (sSYNetStateIsRollbackPending != FALSE)
	{
		u32 from = sSYNetStateRollbackTick;

		sSYNetStateIsRollbackPending = FALSE;

		if (from < cur)
		{
			sSYNetStateRollbackCount++;
			sSYNetStateDiagPhase = nSYNetStatePhaseRollbackResim;
			syNetStateResimulate(from, cur, task_update, tfunc);
		}
	}
	if ((sSYNetStateSyncTestDepth != 0) && (cur >= sSYNetStateSyncTestDepth))
	{
		sSYNetStateDiagPhase = nSYNetStatePhaseSyncTestResim;
		syNetStateRunSyncTest(cur, task_update, tfunc);
	}
	sSYNetStateDiagPhase = nSYNetStatePhaseLiveTick;
	task_update(tfunc);
	sSYNetStateDiagPhase = nSYNetStatePhaseIdle;
	port_watchdog_note_tick();
}
