#include <sys/netinput.h>

#include <sys/netpeer.h>
#include <sys/netstate.h>
#include <sys/taskman.h>

typedef struct SYNetInputSlot
{
	SYNetInputSource source;
	SYNetInputFrame last_confirmed;
	SYNetInputFrame last_published;

} SYNetInputSlot;

SYNetInputSlot sSYNetInputSlots[MAXCONTROLLERS];
SYNetInputFrame sSYNetInputHistory[MAXCONTROLLERS][SYNETINPUT_HISTORY_LENGTH];
SYNetInputFrame sSYNetInputRemoteHistory[MAXCONTROLLERS][SYNETINPUT_HISTORY_LENGTH];
SYNetInputFrame sSYNetInputSavedHistory[MAXCONTROLLERS][SYNETINPUT_HISTORY_LENGTH];
SYNetInputFrame sSYNetInputReplayFrames[MAXCONTROLLERS][SYNETINPUT_REPLAY_MAX_FRAMES];
SYNetInputReplayMetadata sSYNetInputReplayMetadata;
u32 sSYNetInputTick;
u32 sSYNetInputRecordedFrameCount;
sb32 sSYNetInputIsRecording;
sb32 sSYNetInputIsReplayMetadataValid;
/* Source-independent checksum over every frame published for a tick that
 * was actually advanced by syNetInputFuncRead(). Accumulated at the same
 * point sSYNetInputTick is incremented, so a tick re-published while the
 * P2P start barrier holds is counted once, like the recorder. When a limit
 * is set the value at exactly that many ticks is frozen for the caller.
 * sSYNetInputPublishedTickCount counts advances and today moves in lockstep
 * with sSYNetInputTick; anything that rewinds the tick (a future rollback
 * via syNetInputSetTick) must save and restore these four values with it. */
u32 sSYNetInputPublishedChecksum;
u32 sSYNetInputPublishedTickCount;
u32 sSYNetInputPublishedChecksumLimit;
u32 sSYNetInputPublishedChecksumAtLimit;
/* TRUE while sys/netstate.c re-simulates ticks after a rollback: local slots
 * then replay the input they actually had at that tick from sSYNetInputHistory
 * instead of polling hardware, and the per-tick bookkeeping that rollback
 * rewinds explicitly (tick, checksums, last_published) is driven by
 * syNetInputRestoreState() rather than by the live advance. */
sb32 sSYNetInputIsResimulating;
/* Input delay as a local queue. The frame polled from hardware at tick t is
 * scheduled for tick t + sSYNetInputLocalDelay: stored here, applied when
 * that tick is simulated, and sent to the peer tagged with that tick. Both
 * peers therefore apply the same input on the same tick, which is what
 * rollback's misprediction check assumes; the old scheme (apply at t, send
 * as t + delay) was only consistent at delay 0. Indexed by tick like the
 * history rings, so rollback needs no bookkeeping for it. */
SYNetInputFrame sSYNetInputLocalQueue[MAXCONTROLLERS][SYNETINPUT_HISTORY_LENGTH];
u32 sSYNetInputLocalQueuedCount[MAXCONTROLLERS]; /* ticks [0, count) are queued */
u32 sSYNetInputLocalDelay;
/* Ticks [0, count) of a remote slot have confirmed frames. The gap to the
 * current tick is how far ahead of the peer we are running on prediction;
 * sys/netstate.c stalls when it would exceed the snapshot ring. */
u32 sSYNetInputRemoteConfirmedCount[MAXCONTROLLERS];
/* SSB64_NETINPUT_FAKE=1 (PORT debug): local slots play a deterministic
 * synthetic input pattern instead of polling hardware, so two headless
 * instances exercise mispredictions, rollbacks and the gameplay hash. */
sb32 sSYNetInputFakeLocal;
/* Which physical controller port feeds the local slot in a netplay session.
 * The local player's slot is 0 on the host and 1 on the client, but both of
 * them hold their only controller in port 0; -1 = the slot's own port
 * (offline: every slot is its own controller). */
s32 sSYNetInputLocalHardwarePort = -1;

#ifdef PORT
extern char *getenv(const char *name);
extern int atoi(const char *s);
#endif

u32 syNetInputGetTick(void)
{
	return sSYNetInputTick;
}

void syNetInputSetTick(u32 tick)
{
	sSYNetInputTick = tick;
}

sb32 syNetInputCheckPlayer(s32 player)
{
	return ((player >= 0) && (player < MAXCONTROLLERS)) ? TRUE : FALSE;
}

void syNetInputClearFrame(SYNetInputFrame *frame)
{
	frame->tick = 0;
	frame->buttons = 0;
	frame->stick_x = 0;
	frame->stick_y = 0;
	frame->source = nSYNetInputSourceLocal;
	frame->is_predicted = FALSE;
	frame->is_valid = FALSE;
}

void syNetInputMakeFrame(SYNetInputFrame *frame, u32 tick, u16 buttons, s8 stick_x, s8 stick_y, SYNetInputSource source, sb32 is_predicted)
{
	frame->tick = tick;
	frame->buttons = buttons;
	frame->stick_x = stick_x;
	frame->stick_y = stick_y;
	frame->source = source;
	frame->is_predicted = is_predicted;
	frame->is_valid = TRUE;
}

void syNetInputStoreFrame(SYNetInputFrame history[][SYNETINPUT_HISTORY_LENGTH], s32 player, SYNetInputFrame *frame)
{
	history[player][frame->tick % SYNETINPUT_HISTORY_LENGTH] = *frame;
}

sb32 syNetInputGetStoredFrame(SYNetInputFrame history[][SYNETINPUT_HISTORY_LENGTH], s32 player, u32 tick, SYNetInputFrame *out_frame)
{
	SYNetInputFrame *frame;

	if (syNetInputCheckPlayer(player) == FALSE)
	{
		return FALSE;
	}
	frame = &history[player][tick % SYNETINPUT_HISTORY_LENGTH];

	if ((frame->is_valid == FALSE) || (frame->tick != tick))
	{
		return FALSE;
	}
	if (out_frame != NULL)
	{
		*out_frame = *frame;
	}
	return TRUE;
}

void syNetInputReset(void)
{
	s32 player;
	s32 i;

	sSYNetInputTick = 0;
	sSYNetInputRecordedFrameCount = 0;
	sSYNetInputIsRecording = FALSE;
#ifdef PORT
	{
		const char *env = getenv("SSB64_NETINPUT_FAKE");

		sSYNetInputFakeLocal = ((env != NULL) && (atoi(env) != 0)) ? TRUE : FALSE;
	}
#endif
	sSYNetInputIsReplayMetadataValid = FALSE;
	sSYNetInputPublishedChecksum = 2166136261U;
	sSYNetInputPublishedTickCount = 0;
	sSYNetInputPublishedChecksumLimit = 0;
	sSYNetInputPublishedChecksumAtLimit = 0;

	sSYNetInputReplayMetadata.magic = SYNETINPUT_REPLAY_MAGIC;
	sSYNetInputReplayMetadata.version = SYNETINPUT_REPLAY_VERSION;
	sSYNetInputReplayMetadata.scene_kind = 0;
	sSYNetInputReplayMetadata.player_count = 0;
	sSYNetInputReplayMetadata.stage_kind = 0;
	sSYNetInputReplayMetadata.stocks = 0;
	sSYNetInputReplayMetadata.time_limit = 0;
	sSYNetInputReplayMetadata.item_switch = 0;
	sSYNetInputReplayMetadata.item_toggles = 0;
	sSYNetInputReplayMetadata.rng_seed = 0;
	sSYNetInputReplayMetadata.game_type = 0;
	sSYNetInputReplayMetadata.game_rules = 0;
	sSYNetInputReplayMetadata.is_team_battle = FALSE;
	sSYNetInputReplayMetadata.handicap = 0;
	sSYNetInputReplayMetadata.is_team_attack = FALSE;
	sSYNetInputReplayMetadata.is_stage_select = FALSE;
	sSYNetInputReplayMetadata.damage_ratio = 0;
	sSYNetInputReplayMetadata.item_appearance_rate = 0;
	sSYNetInputReplayMetadata.is_not_teamshadows = FALSE;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		sSYNetInputSlots[player].source = nSYNetInputSourceLocal;
		sSYNetInputLocalQueuedCount[player] = 0;
		sSYNetInputRemoteConfirmedCount[player] = 0;
		syNetInputClearFrame(&sSYNetInputSlots[player].last_confirmed);
		syNetInputClearFrame(&sSYNetInputSlots[player].last_published);
		sSYNetInputReplayMetadata.player_kinds[player] = 0;
		sSYNetInputReplayMetadata.fighter_kinds[player] = 0;
		sSYNetInputReplayMetadata.costumes[player] = 0;
		sSYNetInputReplayMetadata.teams[player] = 0;
		sSYNetInputReplayMetadata.handicaps[player] = 0;
		sSYNetInputReplayMetadata.levels[player] = 0;
		sSYNetInputReplayMetadata.shades[player] = 0;

		for (i = 0; i < SYNETINPUT_HISTORY_LENGTH; i++)
		{
			syNetInputClearFrame(&sSYNetInputHistory[player][i]);
			syNetInputClearFrame(&sSYNetInputRemoteHistory[player][i]);
			syNetInputClearFrame(&sSYNetInputSavedHistory[player][i]);
			syNetInputClearFrame(&sSYNetInputLocalQueue[player][i]);
		}
		for (i = 0; i < SYNETINPUT_REPLAY_MAX_FRAMES; i++)
		{
			syNetInputClearFrame(&sSYNetInputReplayFrames[player][i]);
		}
	}
}

void syNetInputStartVSSession(void)
{
	syNetInputReset();
}

void syNetInputSetSlotSource(s32 player, SYNetInputSource source)
{
	if (syNetInputCheckPlayer(player) != FALSE)
	{
		sSYNetInputSlots[player].source = source;
	}
}

SYNetInputSource syNetInputGetSlotSource(s32 player)
{
	if (syNetInputCheckPlayer(player) == FALSE)
	{
		return nSYNetInputSourceLocal;
	}
	return sSYNetInputSlots[player].source;
}

void syNetInputSetRemoteInput(s32 player, u32 tick, u16 buttons, s8 stick_x, s8 stick_y)
{
	SYNetInputFrame frame;

	SYNetInputFrame published;

	if (syNetInputCheckPlayer(player) == FALSE)
	{
		return;
	}
	syNetInputMakeFrame(&frame, tick, buttons, stick_x, stick_y, nSYNetInputSourceRemoteConfirmed, FALSE);
	syNetInputStoreFrame(sSYNetInputRemoteHistory, player, &frame);

	/* Extend the confirmed prefix over any frames that arrived out of order. */
	while (syNetInputGetStoredFrame(sSYNetInputRemoteHistory, player, sSYNetInputRemoteConfirmedCount[player], NULL)
	       != FALSE)
	{
		sSYNetInputRemoteConfirmedCount[player]++;
	}

	/* A confirmed input for a tick we already simulated: if the prediction we
	 * published for it was wrong, the simulation from that tick on is wrong
	 * and netstate must roll back and re-simulate. A correct prediction only
	 * needs the history entry promoted to confirmed. */
	if ((tick < sSYNetInputTick) && (syNetInputGetStoredFrame(sSYNetInputHistory, player, tick, &published) != FALSE)
	    && (published.is_predicted != FALSE))
	{
		if ((published.buttons != buttons) || (published.stick_x != stick_x) || (published.stick_y != stick_y))
		{
			syNetStateRequestRollback(tick);
		}
		else
		{
			sSYNetInputHistory[player][tick % SYNETINPUT_HISTORY_LENGTH].is_predicted = FALSE;
			sSYNetInputHistory[player][tick % SYNETINPUT_HISTORY_LENGTH].source = nSYNetInputSourceRemoteConfirmed;
		}
	}
}

void syNetInputSetSavedInput(s32 player, u32 tick, u16 buttons, s8 stick_x, s8 stick_y)
{
	SYNetInputFrame frame;

	if (syNetInputCheckPlayer(player) != FALSE)
	{
		syNetInputMakeFrame(&frame, tick, buttons, stick_x, stick_y, nSYNetInputSourceSaved, FALSE);
		syNetInputStoreFrame(sSYNetInputSavedHistory, player, &frame);
	}
}

void syNetInputMakeLocalFrame(s32 player, u32 tick, SYNetInputFrame *out_frame)
{
	s32 port = (sSYNetInputLocalHardwarePort >= 0) ? sSYNetInputLocalHardwarePort : player;
	SYController *controller = &gSYControllerDevices[port];

	if (sSYNetInputFakeLocal != FALSE)
	{
		/* Deterministic in (player, tick): a new "decision" every 20 ticks
		 * cycling through walk, jump, attack and idle, so the pattern is
		 * busy enough to move fighters and change often enough to
		 * mispredict at delay 0. Values: A 0x8000, B 0x4000, stick ±60. */
		u32 h = ((tick / 20U) + 1U) * 2654435761U ^ ((u32)player * 40503U);
		u16 buttons = 0;
		s8 stick_x = 0;
		s8 stick_y = 0;

		/* Every special (neutral, side, up, down B), smash and tilt
		 * attacks, jumps, shield and roll: enough to reach each fighter's
		 * state machine, not just walking and jabbing. Bits: A 0x8000,
		 * B 0x4000, Z 0x2000, R 0x0010, C-up 0x0008. */
		switch ((h >> 8) & 15U)
		{
		case 0: stick_x = 60; break;
		case 1: stick_x = -60; break;
		case 2: buttons = 0x8000; break;
		case 3: buttons = 0x4000; stick_x = 60; break;
		case 4: stick_y = 60; break;
		case 5: buttons = 0x8000; stick_x = -60; break;
		case 6: buttons = 0x4000; stick_y = 70; break;   /* up+B */
		case 7: buttons = 0x4000; break;                 /* neutral B */
		case 8: buttons = 0x4000; stick_y = -70; break;  /* down+B */
		case 9: buttons = 0x8000; stick_y = -70; break;  /* down smash / tilt */
		case 10: buttons = 0x0008; stick_x = 40; break;  /* C jump drifting */
		case 11: buttons = 0x0010; break;                /* shield */
		case 12: buttons = 0x0010; stick_x = -60; break; /* roll */
		case 13: buttons = 0x2000; break;                /* Z: grab */
		case 14: stick_y = -70; break;                   /* crouch / platform drop */
		default: break;
		}
		syNetInputMakeFrame(out_frame, tick, buttons, stick_x, stick_y, nSYNetInputSourceLocal, FALSE);
		return;
	}
	syNetInputMakeFrame
	(
		out_frame,
		tick,
		controller->button_hold,
		controller->stick_range.x,
		controller->stick_range.y,
		nSYNetInputSourceLocal,
		FALSE
	);
}

/* Poll hardware for this tick and schedule the result for tick + delay. */
void syNetInputQueueLocalFrame(s32 player, u32 tick)
{
	SYNetInputFrame frame;
	u32 target = tick + sSYNetInputLocalDelay;

	/* The first `delay` ticks of a match have no polled input behind them.
	 * Queue neutral frames for them so the peer receives a frame for every
	 * tick from 0 and its confirmed prefix can start growing; otherwise both
	 * sides sit at "remote lag 9" forever. */
	if (sSYNetInputLocalQueuedCount[player] == 0)
	{
		u32 t;

		for (t = tick; t < target; t++)
		{
			syNetInputMakeFrame(&frame, t, 0, 0, 0, nSYNetInputSourceLocal, FALSE);
			syNetInputStoreFrame(sSYNetInputLocalQueue, player, &frame);
		}
	}
	syNetInputMakeLocalFrame(player, target, &frame);
	syNetInputStoreFrame(sSYNetInputLocalQueue, player, &frame);

	if (sSYNetInputLocalQueuedCount[player] < target + 1)
	{
		sSYNetInputLocalQueuedCount[player] = target + 1;
	}
}

void syNetInputMakePredictedFrame(s32 player, u32 tick, SYNetInputFrame *out_frame)
{
	SYNetInputFrame *last_confirmed = &sSYNetInputSlots[player].last_confirmed;

	if (last_confirmed->is_valid != FALSE)
	{
		syNetInputMakeFrame
		(
			out_frame,
			tick,
			last_confirmed->buttons,
			last_confirmed->stick_x,
			last_confirmed->stick_y,
			nSYNetInputSourceRemotePredicted,
			TRUE
		);
	}
	else syNetInputMakeFrame(out_frame, tick, 0, 0, 0, nSYNetInputSourceRemotePredicted, TRUE);
}

void syNetInputResolveFrame(s32 player, u32 tick, SYNetInputFrame *out_frame)
{
	switch (sSYNetInputSlots[player].source)
	{
	case nSYNetInputSourceRemoteConfirmed:
	case nSYNetInputSourceRemotePredicted:
		if (syNetInputGetStoredFrame(sSYNetInputRemoteHistory, player, tick, out_frame) != FALSE)
		{
			sSYNetInputSlots[player].last_confirmed = *out_frame;
		}
		else syNetInputMakePredictedFrame(player, tick, out_frame);
		break;

	case nSYNetInputSourceSaved:
		if (syNetInputGetReplayFrame(player, tick, out_frame) != FALSE)
		{
			out_frame->source = nSYNetInputSourceSaved;
			out_frame->is_predicted = FALSE;
		}
		else if (syNetInputGetStoredFrame(sSYNetInputSavedHistory, player, tick, out_frame) == FALSE)
		{
			syNetInputMakeFrame(out_frame, tick, 0, 0, 0, nSYNetInputSourceSaved, FALSE);
		}
		break;

	case nSYNetInputSourceLocal:
	default:
		if ((sSYNetInputIsResimulating != FALSE)
		    && (syNetInputGetStoredFrame(sSYNetInputHistory, player, tick, out_frame) != FALSE))
		{
			out_frame->source = nSYNetInputSourceLocal;
			out_frame->is_predicted = FALSE;
			break;
		}
		/* Live tick: queue what the hardware says now for tick + delay, then
		 * apply whatever was queued for this tick (neutral for the first
		 * `delay` ticks of a match). */
		syNetInputQueueLocalFrame(player, tick);

		if (syNetInputGetStoredFrame(sSYNetInputLocalQueue, player, tick, out_frame) != FALSE)
		{
			out_frame->source = nSYNetInputSourceLocal;
			out_frame->is_predicted = FALSE;
		}
		else syNetInputMakeFrame(out_frame, tick, 0, 0, 0, nSYNetInputSourceLocal, FALSE);
		break;
	}
}

void syNetInputPublishFrame(s32 player, SYNetInputFrame *frame)
{
	SYNetInputFrame *last_published = &sSYNetInputSlots[player].last_published;
	u16 prev_buttons = (last_published->is_valid != FALSE) ? last_published->buttons : 0;
	u16 pressed = (frame->buttons ^ prev_buttons) & frame->buttons;
	u16 released = (frame->buttons ^ prev_buttons) & prev_buttons;

	gSYControllerDevices[player].button_hold = frame->buttons;
	gSYControllerDevices[player].button_tap = pressed;
	gSYControllerDevices[player].button_release = released;
	gSYControllerDevices[player].button_update = pressed;
	gSYControllerDevices[player].stick_range.x = frame->stick_x;
	gSYControllerDevices[player].stick_range.y = frame->stick_y;

	sSYNetInputSlots[player].last_published = *frame;
	syNetInputStoreFrame(sSYNetInputHistory, player, frame);
}

void syNetInputPublishMainController(void)
{
	gSYControllerMain.button_hold = gSYControllerDevices[0].button_hold;
	gSYControllerMain.button_tap = gSYControllerDevices[0].button_tap;
	gSYControllerMain.button_update = gSYControllerDevices[0].button_update;
	gSYControllerMain.button_release = gSYControllerDevices[0].button_release;
	gSYControllerMain.stick_range.x = gSYControllerDevices[0].stick_range.x;
	gSYControllerMain.stick_range.y = gSYControllerDevices[0].stick_range.y;
}

sb32 syNetInputGetHistoryFrame(s32 player, u32 tick, SYNetInputFrame *out_frame)
{
	return syNetInputGetStoredFrame(sSYNetInputHistory, player, tick, out_frame);
}

sb32 syNetInputGetPublishedFrame(s32 player, SYNetInputFrame *out_frame)
{
	if (syNetInputCheckPlayer(player) == FALSE)
	{
		return FALSE;
	}
	if (sSYNetInputSlots[player].last_published.is_valid == FALSE)
	{
		return FALSE;
	}
	if (out_frame != NULL)
	{
		*out_frame = sSYNetInputSlots[player].last_published;
	}
	return TRUE;
}

u32 syNetInputGetHistoryChecksum(s32 player, u32 tick_begin, u32 frame_count)
{
	SYNetInputFrame frame;
	u32 checksum = 2166136261U;
	u32 i;

	for (i = 0; i < frame_count; i++)
	{
		if (syNetInputGetHistoryFrame(player, tick_begin + i, &frame) != FALSE)
		{
			checksum ^= frame.tick;
			checksum *= 16777619U;
			checksum ^= frame.buttons;
			checksum *= 16777619U;
			checksum ^= (u8)frame.stick_x;
			checksum *= 16777619U;
			checksum ^= (u8)frame.stick_y;
			checksum *= 16777619U;
			checksum ^= frame.source;
			checksum *= 16777619U;
			checksum ^= frame.is_predicted;
			checksum *= 16777619U;
		}
	}
	return checksum;
}

u32 syNetInputAccumulateInputChecksum(u32 checksum, s32 player, SYNetInputFrame *frame)
{
	checksum ^= (u32)player;
	checksum *= 16777619U;
	checksum ^= frame->tick;
	checksum *= 16777619U;
	checksum ^= frame->buttons;
	checksum *= 16777619U;
	checksum ^= (u8)frame->stick_x;
	checksum *= 16777619U;
	checksum ^= (u8)frame->stick_y;
	checksum *= 16777619U;

	return checksum;
}

u32 syNetInputGetHistoryInputValueChecksumForPlayer(s32 player, u32 tick_begin, u32 frame_count)
{
	SYNetInputFrame frame;
	u32 checksum = 2166136261U;
	u32 i;

	for (i = 0; i < frame_count; i++)
	{
		if (syNetInputGetHistoryFrame(player, tick_begin + i, &frame) != FALSE)
		{
			checksum = syNetInputAccumulateInputChecksum(checksum, player, &frame);
		}
	}
	return checksum;
}

void syNetInputGetHistoryInputValueChecksumWindow(u32 tick_begin, u32 frame_count, u32 *out_checksums,
                                                  u32 *out_combined_checksum)
{
	SYNetInputFrame frame;
	u32 checksum = 2166136261U;
	u32 tick_limit;
	u32 tick;
	s32 player;

	tick_limit = tick_begin + frame_count;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		u32 player_checksum = 2166136261U;

		for (tick = tick_begin; tick < tick_limit; tick++)
		{
			if (syNetInputGetHistoryFrame(player, tick, &frame) != FALSE)
			{
				player_checksum = syNetInputAccumulateInputChecksum(player_checksum, player, &frame);
			}
		}
		checksum ^= player_checksum;
		checksum *= 16777619U;

		if (out_checksums != NULL)
		{
			out_checksums[player] = player_checksum;
		}
	}
	if (out_combined_checksum != NULL)
	{
		*out_combined_checksum = checksum;
	}
}

void syNetInputSetRecordingEnabled(sb32 is_enabled)
{
	sSYNetInputIsRecording = is_enabled;

	if (is_enabled != FALSE)
	{
		sSYNetInputRecordedFrameCount = 0;
	}
}

sb32 syNetInputGetRecordingEnabled(void)
{
	return sSYNetInputIsRecording;
}

u32 syNetInputGetRecordedFrameCount(void)
{
	return sSYNetInputRecordedFrameCount;
}

void syNetInputClearReplayFrames(void)
{
	s32 player;
	s32 i;

	sSYNetInputRecordedFrameCount = 0;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		for (i = 0; i < SYNETINPUT_REPLAY_MAX_FRAMES; i++)
		{
			syNetInputClearFrame(&sSYNetInputReplayFrames[player][i]);
		}
	}
}

sb32 syNetInputSetReplayFrame(s32 player, u32 tick, const SYNetInputFrame *frame)
{
	if ((syNetInputCheckPlayer(player) == FALSE) || (frame == NULL) || (tick >= SYNETINPUT_REPLAY_MAX_FRAMES))
	{
		return FALSE;
	}
	sSYNetInputReplayFrames[player][tick] = *frame;
	sSYNetInputReplayFrames[player][tick].tick = tick;
	sSYNetInputReplayFrames[player][tick].is_valid = TRUE;

	if (sSYNetInputRecordedFrameCount < (tick + 1))
	{
		sSYNetInputRecordedFrameCount = tick + 1;
	}
	return TRUE;
}

sb32 syNetInputGetReplayFrame(s32 player, u32 tick, SYNetInputFrame *out_frame)
{
	SYNetInputFrame *frame;

	if ((syNetInputCheckPlayer(player) == FALSE) || (tick >= SYNETINPUT_REPLAY_MAX_FRAMES))
	{
		return FALSE;
	}
	frame = &sSYNetInputReplayFrames[player][tick];

	if ((frame->is_valid == FALSE) || (frame->tick != tick))
	{
		return FALSE;
	}
	if (out_frame != NULL)
	{
		*out_frame = *frame;
	}
	return TRUE;
}

u32 syNetInputGetReplayInputChecksum(void)
{
	SYNetInputFrame frame;
	u32 checksum = 2166136261U;
	u32 tick;
	s32 player;

	for (tick = 0; tick < sSYNetInputRecordedFrameCount; tick++)
	{
		for (player = 0; player < MAXCONTROLLERS; player++)
		{
			if (syNetInputGetReplayFrame(player, tick, &frame) != FALSE)
			{
				checksum = syNetInputAccumulateInputChecksum(checksum, player, &frame);
			}
		}
	}
	return checksum;
}

void syNetInputSetReplayMetadata(const SYNetInputReplayMetadata *metadata)
{
	if (metadata != NULL)
	{
		sSYNetInputReplayMetadata = *metadata;
		sSYNetInputReplayMetadata.magic = SYNETINPUT_REPLAY_MAGIC;
		sSYNetInputReplayMetadata.version = SYNETINPUT_REPLAY_VERSION;
		sSYNetInputIsReplayMetadataValid = TRUE;
	}
}

sb32 syNetInputGetReplayMetadata(SYNetInputReplayMetadata *out_metadata)
{
	if (sSYNetInputIsReplayMetadataValid == FALSE)
	{
		return FALSE;
	}
	if (out_metadata != NULL)
	{
		*out_metadata = sSYNetInputReplayMetadata;
	}
	return TRUE;
}

void syNetInputFuncRead(void)
{
	SYNetInputFrame frame;
	u32 tick;
	s32 player;

	if (sSYNetInputIsResimulating == FALSE)
	{
		syControllerFuncRead();
	}
	tick = syNetInputGetTick();

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		syNetInputResolveFrame(player, tick, &frame);
		syNetInputPublishFrame(player, &frame);

		if (sSYNetInputIsRecording != FALSE)
		{
			syNetInputSetReplayFrame(player, tick, &frame);
		}
	}
	syNetInputPublishMainController();

	if (syNetPeerCheckStartBarrierReleased() == FALSE)
	{
		return;
	}
	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		sSYNetInputPublishedChecksum = syNetInputAccumulateInputChecksum(
			sSYNetInputPublishedChecksum, player, &sSYNetInputSlots[player].last_published);
	}
	sSYNetInputPublishedTickCount++;

	if ((sSYNetInputPublishedChecksumLimit != 0) && (sSYNetInputPublishedTickCount == sSYNetInputPublishedChecksumLimit))
	{
		sSYNetInputPublishedChecksumAtLimit = sSYNetInputPublishedChecksum;
	}
	sSYNetInputTick++;
}

void syNetInputSetPublishedChecksumLimit(u32 tick_limit)
{
	sSYNetInputPublishedChecksumLimit = tick_limit;
	sSYNetInputPublishedChecksumAtLimit = sSYNetInputPublishedChecksum;
}

u32 syNetInputGetPublishedTickCount(void)
{
	return sSYNetInputPublishedTickCount;
}

u32 syNetInputGetPublishedInputChecksum(void)
{
	if ((sSYNetInputPublishedChecksumLimit != 0) && (sSYNetInputPublishedTickCount >= sSYNetInputPublishedChecksumLimit))
	{
		return sSYNetInputPublishedChecksumAtLimit;
	}
	return sSYNetInputPublishedChecksum;
}

void syNetInputSetLocalDelay(u32 delay)
{
	if (delay >= SYNETINPUT_HISTORY_LENGTH / 2)
	{
		delay = SYNETINPUT_HISTORY_LENGTH / 2 - 1;
	}
	sSYNetInputLocalDelay = delay;
}

u32 syNetInputGetLocalDelay(void)
{
	return sSYNetInputLocalDelay;
}

sb32 syNetInputGetLocalQueuedFrame(s32 player, u32 tick, SYNetInputFrame *out_frame)
{
	return syNetInputGetStoredFrame(sSYNetInputLocalQueue, player, tick, out_frame);
}

u32 syNetInputGetLocalQueuedCount(s32 player)
{
	if (syNetInputCheckPlayer(player) == FALSE)
	{
		return 0;
	}
	return sSYNetInputLocalQueuedCount[player];
}

u32 syNetInputGetRemoteConfirmedCount(s32 player)
{
	if (syNetInputCheckPlayer(player) == FALSE)
	{
		return 0;
	}
	return sSYNetInputRemoteConfirmedCount[player];
}

/* Ticks the simulation has run past the newest fully-confirmed remote tick,
 * over every remote slot: how deep a rollback could still have to reach. */
u32 syNetInputGetRemoteLag(void)
{
	u32 lag = 0;
	s32 player;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		SYNetInputSource source = sSYNetInputSlots[player].source;

		if ((source == nSYNetInputSourceRemoteConfirmed) || (source == nSYNetInputSourceRemotePredicted))
		{
			u32 count = sSYNetInputRemoteConfirmedCount[player];

			if ((sSYNetInputTick > count) && (sSYNetInputTick - count > lag))
			{
				lag = sSYNetInputTick - count;
			}
		}
	}
	return lag;
}

void syNetInputSetLocalHardwarePort(s32 port)
{
	sSYNetInputLocalHardwarePort = ((port >= 0) && (port < MAXCONTROLLERS)) ? port : -1;
}

sb32 syNetInputGetRemoteFrame(s32 player, u32 tick, SYNetInputFrame *out_frame)
{
	return syNetInputGetStoredFrame(sSYNetInputRemoteHistory, player, tick, out_frame);
}

void syNetInputSetResimulating(sb32 is_resimulating)
{
	sSYNetInputIsResimulating = is_resimulating;
}

sb32 syNetInputGetResimulating(void)
{
	return sSYNetInputIsResimulating;
}

void syNetInputSaveState(SYNetInputState *out_state)
{
	s32 player;

	out_state->tick = sSYNetInputTick;
	out_state->published_checksum = sSYNetInputPublishedChecksum;
	out_state->published_tick_count = sSYNetInputPublishedTickCount;
	out_state->published_checksum_at_limit = sSYNetInputPublishedChecksumAtLimit;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		out_state->last_confirmed[player] = sSYNetInputSlots[player].last_confirmed;
		out_state->last_published[player] = sSYNetInputSlots[player].last_published;
	}
}

void syNetInputRestoreState(const SYNetInputState *state)
{
	s32 player;

	sSYNetInputTick = state->tick;
	sSYNetInputPublishedChecksum = state->published_checksum;
	sSYNetInputPublishedTickCount = state->published_tick_count;
	sSYNetInputPublishedChecksumAtLimit = state->published_checksum_at_limit;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		sSYNetInputSlots[player].last_confirmed = state->last_confirmed[player];
		sSYNetInputSlots[player].last_published = state->last_published[player];
	}
}
