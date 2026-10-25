#include <sys/netpeer.h>

#include <ft/fighter.h>
#include <gr/ground.h>
#include <sc/scmanager.h>
#include <sys/netinput.h>
#include <sys/netmenu.h>
#include <sys/netreplay.h>
#include <sys/netstate.h>
#include <sys/netsync.h>
#include <sys/utils.h>

#ifdef PORT
extern char *getenv(const char *name);
extern int atoi(const char *s);
extern void port_log(const char *fmt, ...);
#endif

#ifdef PORT
/* The UDP socket itself is behind port/netsock.h so this file is the same
 * on POSIX and WinSock. */
#include <netsock.h>
#include <string.h>
#endif

#define SYNETPEER_MAGIC 0x53534E50 // SSNP
#define SYNETPEER_VERSION 5 // 5: up to four members per session through the host, rounds, spectators
#define SYNETPEER_MAX_PACKET_FRAMES 16
#define SYNETPEER_FRAME_BYTES 8
#define SYNETPEER_INPUT_HEADER_BYTES (4 + 2 + 2 + 4 + 4 + 4 + 1 + 1 + 1 + 1)
#define SYNETPEER_PACKET_BYTES ((SYNETPEER_INPUT_HEADER_BYTES) + ((SYNETPEER_MAX_PACKET_FRAMES) * (SYNETPEER_FRAME_BYTES)) + 4)
#define SYNETPEER_VALIDATION_INPUT_WINDOW 120
#define SYNETPEER_METADATA_BYTES (11 * 4 + 8 + (MAXCONTROLLERS * 7))
#define SYNETPEER_BOOTSTRAP_PACKET_BYTES (4 + 2 + 2 + 4 + SYNETPEER_METADATA_BYTES + 4 + 4 + 4)
#define SYNETPEER_STOP_REPEAT_PACKETS 5
#define SYNETPEER_CONTROL_PACKET_BYTES (4 + 2 + 2 + 4 + 4)
#define SYNETPEER_DEFAULT_INPUT_DELAY 2
#define SYNETPEER_DEFAULT_SESSION_ID 1
#define SYNETPEER_DEFAULT_BOOTSTRAP_SEED 12345
#define SYNETPEER_LOG_INTERVAL 120
#define SYNETPEER_BOOTSTRAP_RETRY_COUNT 1800 /* 30 s: the other machine may still be booting */
#define SYNETPEER_BOOTSTRAP_RETRY_USECS 16666
#define SYNETPEER_BARRIER_LOG_INTERVAL 30
#define SYNETPEER_BATTLE_START_REPEAT_FRAMES 30

#define SYNETPEER_PACKET_INPUT 0
#define SYNETPEER_PACKET_MATCH_CONFIG 1
#define SYNETPEER_PACKET_READY 2
#define SYNETPEER_PACKET_START 3
#define SYNETPEER_PACKET_BATTLE_READY 4
#define SYNETPEER_PACKET_BATTLE_START 5
#define SYNETPEER_PACKET_PING 6
#define SYNETPEER_PACKET_PONG 7
#define SYNETPEER_PACKET_REMATCH 8 /* ping-sized; the stamp field carries the sender's fighter kind */
#define SYNETPEER_REMATCH_INTERVAL_FRAMES 30
/* PING/PONG: control header + a u32 millisecond stamp the peer echoes back. */
#define SYNETPEER_PING_PACKET_BYTES (4 + 2 + 2 + 4 + 4 + 4)
#define SYNETPEER_PING_INTERVAL_FRAMES 30

/* ---- Groups: up to four people in one session ----
 * The host is the hub. Every guest talks to the host only; the host sends
 * its own packets to every guest and forwards each guest's input packets to
 * the others, so nobody needs to reach anyone but the host. Each person is a
 * "member" (0 = the host). Who plays which port and who watches is decided
 * per round: a round starts with the host's ROUND_CONFIG (roster, rules,
 * seed, the scene to reset to), every member answers READY, the host sends
 * ROUND_START and everyone resets into the character select (the VS menu for
 * the first round) in lockstep. A round ends when the results screen does;
 * the host rotates (winner stays on), takes in anyone who joined meanwhile,
 * drops anyone who left, and starts the next one. Spectators receive all
 * the players' inputs and simulate the same game, only ever on confirmed
 * input (sys/netstate.c), and never send input, so nobody waits for them. */
#define SYNETPEER_MAX_MEMBERS 4
#define SYNETPEER_CODE_BYTES 16
#define SYNETPEER_PACKET_ROUND_CONFIG 9
#define SYNETPEER_PACKET_ROUND_START 10 /* control-sized */
#define SYNETPEER_PACKET_BYE 11         /* control-sized, any session */
#define SYNETPEER_ROSTER_ENTRY_BYTES (1 + 1 + 1 + 1 + SYNETPEER_CODE_BYTES)
#define SYNETPEER_ROUND_PACKET_BYTES                                                                                  \
	(4 + 2 + 2 + 4 + 4 + 4 + 1 + 1 + 1 + 1 + 1 + 4 + (SYNETPEER_MAX_MEMBERS * SYNETPEER_ROSTER_ENTRY_BYTES) +        \
	 SYNETPEER_METADATA_BYTES + 4 + 4 + 4)
#define SYNETPEER_MAX_DATAGRAM 512
#define SYNETPEER_MAX_RECEIVE_PER_FRAME 256
#define SYNETPEER_ROUND_RESEND_FRAMES 6
#define SYNETPEER_ROUND_START_REPEAT 30
#define SYNETPEER_ROUND_MEMBER_TIMEOUT_MS 10000
#define SYNETPEER_SILENT_MS 5000         /* a member this quiet while we wait on them is gone */
#define SYNETPEER_HOST_SILENT_MS 15000   /* a guest gives up on a host this quiet */
#define SYNETPEER_EARLY_FRAMES 256
#define SYNETPEER_ROUND_FLAG_PRESET 0x1
#define SYNETPEER_SLOT_NONE 0xFF

typedef struct SYNetPeerPacketFrame
{
	u32 tick;
	u16 buttons;
	s8 stick_x;
	s8 stick_y;

} SYNetPeerPacketFrame;

sb32 sSYNetPeerIsEnabled;
sb32 sSYNetPeerIsConfigured;
sb32 sSYNetPeerIsActive;
s32 sSYNetPeerLocalPlayer;
s32 sSYNetPeerRemotePlayer;
u32 sSYNetPeerInputDelay;
u32 sSYNetPeerSessionID;
u32 sSYNetPeerHighestRemoteTick;
u32 sSYNetPeerPacketsSent;
u32 sSYNetPeerPacketsReceived;
u32 sSYNetPeerPacketsDropped;
u32 sSYNetPeerFramesStaged;
u32 sSYNetPeerLateFrames;
u32 sSYNetPeerInputChecksum;
u32 sSYNetPeerLastLogTick;
u32 sSYNetPeerSendSeq;
u32 sSYNetPeerRecvSeqHighWater;
sb32 sSYNetPeerRecvSeqInitialized;
u32 sSYNetPeerSeqGaps;
u32 sSYNetPeerSeqDuplicates;
u32 sSYNetPeerSeqOutOfOrder;
u32 sSYNetPeerLastPeerAckTick;
u32 sSYNetPeerLastPacketOldestTick;
u32 sSYNetPeerLastPacketNewestTick;
sb32 sSYNetPeerLastPacketTicksValid;
/* Time sync (GGPO-style frame advantage). Local advantage = how many ticks
 * our simulation is ahead of the newest input we have from the peer; each
 * packet carries the sender's value so both sides can compare. The side
 * that is further ahead skips the occasional tick (sys/netstate.c) so the
 * two run in step instead of one of them predicting deep and stalling. */
s32 sSYNetPeerRemoteAdvantage;
sb32 sSYNetPeerRemoteAdvantageValid;
/* Round trip, smoothed; 0 until the first PONG. */
u32 sSYNetPeerPingMs;
u32 sSYNetPeerPingFrames;
sb32 sSYNetPeerBootstrapIsEnabled;
sb32 sSYNetPeerBootstrapIsHost;
sb32 sSYNetPeerBootstrapMetadataApplied;
sb32 sSYNetPeerBootstrapPeerReady;
sb32 sSYNetPeerBootstrapStartReceived;
u32 sSYNetPeerBootstrapSeed;
SYNetInputReplayMetadata sSYNetPeerBootstrapMetadata;
sb32 sSYNetPeerBattleBarrierEnabled;
sb32 sSYNetPeerBattleLocalReady;
sb32 sSYNetPeerBattlePeerReady;
sb32 sSYNetPeerBattleStartSent;
sb32 sSYNetPeerBattleStartReceived;
sb32 sSYNetPeerBattleBarrierReleased;
u32 sSYNetPeerBattleBarrierWaitFrames;
u32 sSYNetPeerBattleStartRepeatFrames;
u32 sSYNetPeerExecutionHoldFrames;
sb32 sSYNetPeerExecutionBeginLogged;

#ifdef PORT
intptr_t sSYNetPeerSocket = -1;
PortNetAddr sSYNetPeerBindAddress;
PortNetAddr sSYNetPeerPeerAddress;
/* Lobby-driven session (syNetPeerLobbyConfigure / syNetPeerLobbyStep). */
sb32 sSYNetPeerLobbyActive;
sb32 sSYNetPeerLobbySession;   /* the socket belongs to a lobby match (kept open between games) */
s32 sSYNetPeerLobbyPhase;
u32 sSYNetPeerLobbyFrames;
u32 sSYNetPeerLobbyStartRepeat;
sb32 sSYNetPeerLobbyHasPicks;
u8 sSYNetPeerLobbyFighters[2];
u32 sSYNetPeerLobbyStageKind;
u32 sSYNetPeerLobbyDelay;
u32 sSYNetPeerLobbyStage;
u32 sSYNetPeerLobbyStocks;       /* 0 = not a stock match */
u32 sSYNetPeerLobbyTimeMinutes;  /* 0 = no time limit */
/* Between games of a lobby match: who wants another one. */
sb32 sSYNetPeerPostMatch;
sb32 sSYNetPeerRematchLocal;
sb32 sSYNetPeerRematchPeer;
u8 sSYNetPeerRematchLocalFighter;
u8 sSYNetPeerRematchPeerFighter;
u32 sSYNetPeerRematchFrames;
/* In-menu sessions (sys/netmenu.c): the players pick everything in the
 * game's own VS menus, run in lockstep, and each battle starts with a
 * blocking exchange of the host's battle state. Every menu phase and every
 * battle gets its own session id (base + phase) so a packet from the
 * previous phase can never be taken for one of the current tick space. */
sb32 sSYNetPeerMenuMode;
u32 sSYNetPeerSessionBase;
u32 sSYNetPeerSessionPhase;
/* The client plays with the host's unlocks for the duration of the session
 * so both character selects show the same roster; put back afterwards. */
sb32 sSYNetPeerMasksOverridden;
u8 sSYNetPeerSavedUnlockMask;
u16 sSYNetPeerSavedFighterMask;
u32 sSYNetPeerPeerUnlockMask;
u32 sSYNetPeerPeerFighterMask;
extern void port_coroutine_yield(void);

typedef struct SYNetPeerMember
{
	sb32 is_used;      /* host: a guest that reached us; guest: entry 0 is the host, the rest roster only */
	sb32 is_in_round;  /* takes part in the current round, as a player or watching */
	PortNetAddr addr;  /* host: the guest's; guest: only entry 0 (the host) */
	char code[SYNETPEER_CODE_BYTES];
	s32 slot;          /* player slot this round, -1 = watching */
	s32 queue_pos;     /* winner-stays-on queue position, -1 = not waiting */
	u32 last_heard_ms;
	u32 ping_ms;
	sb32 is_ready;     /* READY for the exchange of the current session id */
	sb32 is_battle_ready;
	sb32 is_battle_start;

} SYNetPeerMember;

SYNetPeerMember sSYNetPeerMembers[SYNETPEER_MAX_MEMBERS];
s32 sSYNetPeerMyMember;
sb32 sSYNetPeerIsHub;          /* we are the host of a session: every packet goes through us */
u32 sSYNetPeerRemoteMask;      /* player slots someone else drives this round */
s32 sSYNetPeerSlotOwner[MAXCONTROLLERS];
u32 sSYNetPeerSlotHighestTick[MAXCONTROLLERS];
sb32 sSYNetPeerSlotHasTick[MAXCONTROLLERS];
s32 sSYNetPeerSlotAdvantage[MAXCONTROLLERS];
sb32 sSYNetPeerSlotAdvantageValid[MAXCONTROLLERS];
sb32 sSYNetPeerBattleRunning;  /* a menu-mode battle is between its start and stop */
u8 sSYNetPeerGroupMode;        /* SYNETPEER_MODE_* of the current round */
u8 sSYNetPeerRoundFlags;
/* Winner stays on (host): who holds ports 1 and 2, who is waiting, in order. */
s32 sSYNetPeerRotPlayers[2];
s32 sSYNetPeerRotKing;
s32 sSYNetPeerQueue[SYNETPEER_MAX_MEMBERS];
s32 sSYNetPeerQueueCount;
u8 sSYNetPeerRotMode;
sb32 sSYNetPeerRotValid;
/* Round exchange. Host: 1 = collecting READY. Guest: 1 = holding a config,
 * READY sent, waiting for ROUND_START. */
s32 sSYNetPeerRoundState;
sb32 sSYNetPeerRoundApplyPending;
sb32 sSYNetPeerRoundAwaiting;  /* guest: at a round boundary, waiting for the host's next round */
sb32 sSYNetPeerRoundFailed;
u32 sSYNetPeerRoundsApplied;
u32 sSYNetPeerRoundFrames;
u32 sSYNetPeerRoundStartMs;
u32 sSYNetPeerRoundStartRepeat;
u32 sSYNetPeerRoundSession;
u32 sSYNetPeerLastPingMs;      /* host-silence clock for a guest's own pings */
/* The round being set up (host: built; guest: as received). */
typedef struct SYNetPeerRound
{
	u32 session_id;
	u32 session_base;
	u32 session_phase;
	u8 your_member;
	u8 mode;
	u8 flags;
	u8 scene;
	u8 scene_prev;
	u32 seed;
	u8 used[SYNETPEER_MAX_MEMBERS];
	u8 in_round[SYNETPEER_MAX_MEMBERS];
	u8 slot[SYNETPEER_MAX_MEMBERS];
	u8 queue_pos[SYNETPEER_MAX_MEMBERS];
	char code[SYNETPEER_MAX_MEMBERS][SYNETPEER_CODE_BYTES];
	SYNetInputReplayMetadata metadata;
	u32 unlock_mask;
	u32 fighter_mask;

} SYNetPeerRound;

SYNetPeerRound sSYNetPeerRound;
/* Input frames for the next session that arrive before we get there (a
 * player already on the results screen while we finish the battle). */
u32 sSYNetPeerEarlySession;
SYNetPeerPacketFrame sSYNetPeerEarlyFrames[MAXCONTROLLERS][SYNETPEER_EARLY_FRAMES];
sb32 sSYNetPeerEarlyValid[MAXCONTROLLERS][SYNETPEER_EARLY_FRAMES];

/* port/netplay/lobby.cpp */
extern void port_lobby_host_foreign_datagram(intptr_t sock, const u8 *buffer, s32 size, const PortNetAddr *from);
extern s32 port_lobby_group_mode(void);
extern s32 port_lobby_preset(void);
extern const char *port_lobby_code(void);
extern void port_lobby_notify_round(void);
/* port/enhancements/DisableStageHazards.cpp */
extern void port_netplay_set_hazards_off(s32 is_off);
/* mn/mnvsmode/mnvsresults.c */
extern s32 portMNVSResultsGetPlace(s32 player);
extern void portSCManagerRequestResetTo(s32 scene, s32 scene_prev);

void syNetPeerApplySlots(void);
void syNetPeerHostStartRound(sb32 after_match, s32 scene, s32 scene_prev);
#endif

u32 syNetPeerChecksumAccumulateU32(u32 checksum, u32 value)
{
	checksum ^= value;
	checksum *= 16777619U;

	return checksum;
}

u32 syNetPeerChecksumAccumulateFrame(u32 checksum, const SYNetPeerPacketFrame *frame)
{
	checksum = syNetPeerChecksumAccumulateU32(checksum, frame->tick);
	checksum = syNetPeerChecksumAccumulateU32(checksum, frame->buttons);
	checksum = syNetPeerChecksumAccumulateU32(checksum, (u8)frame->stick_x);
	checksum = syNetPeerChecksumAccumulateU32(checksum, (u8)frame->stick_y);

	return checksum;
}

u32 syNetPeerChecksumPacket(u32 session_id, u32 ack_tick, u32 packet_seq, u8 player, u8 frame_count, const SYNetPeerPacketFrame *frames)
{
	u32 checksum = 2166136261U;
	s32 i;

	checksum = syNetPeerChecksumAccumulateU32(checksum, SYNETPEER_MAGIC);
	checksum = syNetPeerChecksumAccumulateU32(checksum, SYNETPEER_VERSION);
	checksum = syNetPeerChecksumAccumulateU32(checksum, SYNETPEER_PACKET_INPUT);
	checksum = syNetPeerChecksumAccumulateU32(checksum, session_id);
	checksum = syNetPeerChecksumAccumulateU32(checksum, ack_tick);
	checksum = syNetPeerChecksumAccumulateU32(checksum, packet_seq);
	checksum = syNetPeerChecksumAccumulateU32(checksum, player);
	checksum = syNetPeerChecksumAccumulateU32(checksum, frame_count);

	for (i = 0; i < frame_count; i++)
	{
		checksum = syNetPeerChecksumAccumulateFrame(checksum, &frames[i]);
	}
	return checksum;
}

u32 syNetPeerChecksumBytes(const u8 *bytes, u32 size)
{
	u32 checksum = 2166136261U;
	u32 i;

	for (i = 0; i < size; i++)
	{
		checksum ^= bytes[i];
		checksum *= 16777619U;
	}
	return checksum;
}

void syNetPeerWriteU8(u8 **cursor, u8 value)
{
	*(*cursor)++ = value;
}

void syNetPeerWriteU16(u8 **cursor, u16 value)
{
	*(*cursor)++ = (value >> 8) & 0xFF;
	*(*cursor)++ = value & 0xFF;
}

void syNetPeerWriteU32(u8 **cursor, u32 value)
{
	*(*cursor)++ = (value >> 24) & 0xFF;
	*(*cursor)++ = (value >> 16) & 0xFF;
	*(*cursor)++ = (value >> 8) & 0xFF;
	*(*cursor)++ = value & 0xFF;
}

u8 syNetPeerReadU8(const u8 **cursor)
{
	return *(*cursor)++;
}

u16 syNetPeerReadU16(const u8 **cursor)
{
	u16 value = (u16)(*(*cursor)++) << 8;

	value |= *(*cursor)++;

	return value;
}

u32 syNetPeerReadU32(const u8 **cursor)
{
	u32 value = (u32)(*(*cursor)++) << 24;

	value |= (u32)(*(*cursor)++) << 16;
	value |= (u32)(*(*cursor)++) << 8;
	value |= *(*cursor)++;

	return value;
}

void syNetPeerWriteMetadata(u8 **cursor, const SYNetInputReplayMetadata *metadata)
{
	s32 player;

	syNetPeerWriteU32(cursor, metadata->magic);
	syNetPeerWriteU32(cursor, metadata->version);
	syNetPeerWriteU32(cursor, metadata->scene_kind);
	syNetPeerWriteU32(cursor, metadata->player_count);
	syNetPeerWriteU32(cursor, metadata->stage_kind);
	syNetPeerWriteU32(cursor, metadata->stocks);
	syNetPeerWriteU32(cursor, metadata->time_limit);
	syNetPeerWriteU32(cursor, metadata->item_switch);
	syNetPeerWriteU32(cursor, metadata->item_toggles);
	syNetPeerWriteU32(cursor, metadata->rng_seed);
	syNetPeerWriteU32(cursor, metadata->game_type);
	syNetPeerWriteU8(cursor, metadata->game_rules);
	syNetPeerWriteU8(cursor, metadata->is_team_battle);
	syNetPeerWriteU8(cursor, metadata->handicap);
	syNetPeerWriteU8(cursor, metadata->is_team_attack);
	syNetPeerWriteU8(cursor, metadata->is_stage_select);
	syNetPeerWriteU8(cursor, metadata->damage_ratio);
	syNetPeerWriteU8(cursor, metadata->item_appearance_rate);
	syNetPeerWriteU8(cursor, metadata->is_not_teamshadows);

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		syNetPeerWriteU8(cursor, metadata->player_kinds[player]);
		syNetPeerWriteU8(cursor, metadata->fighter_kinds[player]);
		syNetPeerWriteU8(cursor, metadata->costumes[player]);
		syNetPeerWriteU8(cursor, metadata->teams[player]);
		syNetPeerWriteU8(cursor, metadata->handicaps[player]);
		syNetPeerWriteU8(cursor, metadata->levels[player]);
		syNetPeerWriteU8(cursor, metadata->shades[player]);
	}
}

void syNetPeerReadMetadata(const u8 **cursor, SYNetInputReplayMetadata *metadata)
{
	s32 player;

	metadata->magic = syNetPeerReadU32(cursor);
	metadata->version = syNetPeerReadU32(cursor);
	metadata->scene_kind = syNetPeerReadU32(cursor);
	metadata->player_count = syNetPeerReadU32(cursor);
	metadata->stage_kind = syNetPeerReadU32(cursor);
	metadata->stocks = syNetPeerReadU32(cursor);
	metadata->time_limit = syNetPeerReadU32(cursor);
	metadata->item_switch = syNetPeerReadU32(cursor);
	metadata->item_toggles = syNetPeerReadU32(cursor);
	metadata->rng_seed = syNetPeerReadU32(cursor);
	metadata->game_type = syNetPeerReadU32(cursor);
	metadata->game_rules = syNetPeerReadU8(cursor);
	metadata->is_team_battle = syNetPeerReadU8(cursor);
	metadata->handicap = syNetPeerReadU8(cursor);
	metadata->is_team_attack = syNetPeerReadU8(cursor);
	metadata->is_stage_select = syNetPeerReadU8(cursor);
	metadata->damage_ratio = syNetPeerReadU8(cursor);
	metadata->item_appearance_rate = syNetPeerReadU8(cursor);
	metadata->is_not_teamshadows = syNetPeerReadU8(cursor);

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		metadata->player_kinds[player] = syNetPeerReadU8(cursor);
		metadata->fighter_kinds[player] = syNetPeerReadU8(cursor);
		metadata->costumes[player] = syNetPeerReadU8(cursor);
		metadata->teams[player] = syNetPeerReadU8(cursor);
		metadata->handicaps[player] = syNetPeerReadU8(cursor);
		metadata->levels[player] = syNetPeerReadU8(cursor);
		metadata->shades[player] = syNetPeerReadU8(cursor);
	}
}

sb32 syNetPeerCheckMetadata(const SYNetInputReplayMetadata *metadata)
{
	if ((metadata->magic != SYNETINPUT_REPLAY_MAGIC) ||
		(metadata->version != SYNETINPUT_REPLAY_VERSION) ||
		(metadata->scene_kind != nSCKindVSBattle) ||
		(metadata->player_count == 0) ||
		(metadata->player_count > MAXCONTROLLERS))
	{
		return FALSE;
	}
	return TRUE;
}

#ifdef PORT
void syNetPeerSleepBootstrapRetry(void)
{
	/* An in-menu session exchanges the battle state from inside the battle
	 * scene's start function. Yielding the game coroutine there is not
	 * safe (the previous scene's GObjs were still being ejected around the
	 * yield and the second battle of a session crashed in
	 * ftManagerSetupFilesAllKind), so the whole process sleeps instead; on a
	 * normal connection the peer answers within a frame or two. */
	port_netsock_sleep_us(SYNETPEER_BOOTSTRAP_RETRY_USECS);
}

sb32 syNetPeerParseIPv4Address(const char *text, PortNetAddr *out_address)
{
	return (port_netsock_parse_addr(text, out_address) != 0) ? TRUE : FALSE;
}

void syNetPeerCloseSocket(void)
{
	if (sSYNetPeerSocket >= 0)
	{
		port_netsock_close(sSYNetPeerSocket);
		sSYNetPeerSocket = -1;
	}
}

sb32 syNetPeerOpenSocket(void)
{
	if (sSYNetPeerSocket >= 0)
	{
		return TRUE;
	}
	sSYNetPeerSocket = port_netsock_open_udp(&sSYNetPeerBindAddress);

	return (sSYNetPeerSocket >= 0) ? TRUE : FALSE;
}

sb32 syNetPeerCheckSameAddr(const PortNetAddr *a, const PortNetAddr *b)
{
	return ((a->ip == b->ip) && (a->port == b->port)) ? TRUE : FALSE;
}

/* Which member a datagram came from: the host hears from its guests, a
 * guest only from the host (entry 0). -1 = a stranger. */
s32 syNetPeerFindMember(const PortNetAddr *from)
{
	s32 i;

	if (sSYNetPeerIsHub == FALSE)
	{
		return ((sSYNetPeerMembers[0].is_used != FALSE) && (syNetPeerCheckSameAddr(from, &sSYNetPeerMembers[0].addr) != FALSE)) ? 0 : -1;
	}
	for (i = 1; i < SYNETPEER_MAX_MEMBERS; i++)
	{
		if ((sSYNetPeerMembers[i].is_used != FALSE) && (syNetPeerCheckSameAddr(from, &sSYNetPeerMembers[i].addr) != FALSE))
		{
			return i;
		}
	}
	return -1;
}

void syNetPeerHandleDatagram(const u8 *buffer, s32 size, s32 member);

/* Drain every queued datagram. A host passes anything that is not ours
 * (the lobby's PUNCH of someone joining late) to the lobby. */
void syNetPeerReceiveAll(void)
{
	u8 buffer[SYNETPEER_MAX_DATAGRAM];
	s32 count;

	if (sSYNetPeerSocket < 0)
	{
		return;
	}
	/* Bounded, so a flood (or a packet loop) can't hold the frame up. */
	for (count = 0; count < SYNETPEER_MAX_RECEIVE_PER_FRAME; count++)
	{
		PortNetAddr from;
		s32 size = port_netsock_recvfrom(sSYNetPeerSocket, buffer, sizeof(buffer), &from);
		s32 member;
		sb32 is_ours;

		if (size <= 0)
		{
			if (size < 0)
			{
				sSYNetPeerPacketsDropped++;
			}
			break;
		}
		is_ours = ((size >= 4) && (buffer[0] == 'S') && (buffer[1] == 'S') && (buffer[2] == 'N') && (buffer[3] == 'P')) ? TRUE : FALSE;
		member = syNetPeerFindMember(&from);

		if (member >= 0)
		{
			sSYNetPeerMembers[member].last_heard_ms = port_netsock_now_ms();
		}
		if (is_ours == FALSE)
		{
			if (sSYNetPeerIsHub != FALSE)
			{
				port_lobby_host_foreign_datagram(sSYNetPeerSocket, buffer, size, &from);
			}
			continue;
		}
		if (member < 0)
		{
			sSYNetPeerPacketsDropped++;
			continue;
		}
		syNetPeerHandleDatagram(buffer, size, member);
	}
}

void syNetPeerSendToMember(s32 member, const u8 *buffer, u32 size)
{
	port_netsock_sendto(sSYNetPeerSocket, buffer, (s32)size, &sSYNetPeerMembers[member].addr);
}

/* To everyone in the round: a guest has only the host to talk to, the host
 * talks to each guest. except = a member to leave out (-1 for none). */
void syNetPeerSendToRound(const u8 *buffer, u32 size, s32 except)
{
	s32 i;

	if (sSYNetPeerIsHub == FALSE)
	{
		if ((sSYNetPeerMembers[0].is_used != FALSE) && (except != 0))
		{
			syNetPeerSendToMember(0, buffer, size);
		}
		return;
	}
	for (i = 1; i < SYNETPEER_MAX_MEMBERS; i++)
	{
		if ((i != except) && (sSYNetPeerMembers[i].is_used != FALSE) && (sSYNetPeerMembers[i].is_in_round != FALSE))
		{
			syNetPeerSendToMember(i, buffer, size);
		}
	}
}

/* Pings go to every member, in the round or still waiting to get in. */
void syNetPeerSendToAll(const u8 *buffer, u32 size)
{
	s32 i;

	if (sSYNetPeerIsHub == FALSE)
	{
		if (sSYNetPeerMembers[0].is_used != FALSE)
		{
			syNetPeerSendToMember(0, buffer, size);
		}
		return;
	}
	for (i = 1; i < SYNETPEER_MAX_MEMBERS; i++)
	{
		if (sSYNetPeerMembers[i].is_used != FALSE)
		{
			syNetPeerSendToMember(i, buffer, size);
		}
	}
}

/* Host: every other member of the round has the flag set. */
sb32 syNetPeerCheckRoundMembers(s32 flag, sb32 players_only)
{
	s32 i;

	for (i = 1; i < SYNETPEER_MAX_MEMBERS; i++)
	{
		SYNetPeerMember *m = &sSYNetPeerMembers[i];
		sb32 value;

		if ((m->is_used == FALSE) || (m->is_in_round == FALSE) || ((players_only != FALSE) && (m->slot < 0)))
		{
			continue;
		}
		value = (flag == 0) ? m->is_ready : (flag == 1) ? m->is_battle_ready : m->is_battle_start;

		if (value == FALSE)
		{
			return FALSE;
		}
	}
	return TRUE;
}

u32 syNetPeerMemberSilentMs(s32 member)
{
	return port_netsock_now_ms() - sSYNetPeerMembers[member].last_heard_ms;
}
#endif

void syNetPeerMakeBootstrapMetadata(SYNetInputReplayMetadata *metadata)
{
	s32 player;

#ifdef PORT
	if (sSYNetPeerMenuMode != FALSE)
	{
		/* An in-menu session carries the real battle state: the VS rules
		 * when the session starts (so both sides enter the menus from the
		 * host's settings) and, at each battle, the picks the menus made. */
		syNetReplayCaptureBattleMetadata(&gSCManagerTransferBattleState, metadata);
		metadata->rng_seed = sSYNetPeerBootstrapSeed;

		if (metadata->player_count == 0)
		{
			metadata->player_count = 2;
		}
		/* The lobby's preset (Dream Land, no items; hazards are off through
		 * port_netplay_set_hazards_off) wins over whatever the VS options
		 * were set to in the menus. */
		if (sSYNetPeerRoundFlags & SYNETPEER_ROUND_FLAG_PRESET)
		{
			metadata->stage_kind = nGRKindPupupu;
			metadata->is_stage_select = FALSE;
			metadata->item_switch = nSCBattleItemSwitchNone;
			metadata->item_appearance_rate = nSCBattleItemSwitchNone;
		}
		return;
	}
#endif
	memset(metadata, 0, sizeof(*metadata));
	metadata->magic = SYNETINPUT_REPLAY_MAGIC;
	metadata->version = SYNETINPUT_REPLAY_VERSION;
	metadata->scene_kind = nSCKindVSBattle;
	metadata->player_count = 2;
	metadata->stage_kind = nGRKindCastle;
	metadata->stocks = 3;
	metadata->time_limit = SCBATTLE_TIMELIMIT_INFINITE;
	metadata->item_switch = nSCBattleItemSwitchNone;
	metadata->item_toggles = 0;
	metadata->rng_seed = sSYNetPeerBootstrapSeed;
	metadata->game_type = nSCBattleGameTypeRoyal;
	metadata->game_rules = SCBATTLE_GAMERULE_STOCK;
	metadata->is_team_battle = FALSE;
	metadata->handicap = nSCBattleHandicapOff;
	metadata->is_team_attack = FALSE;
	metadata->is_stage_select = FALSE;
	metadata->damage_ratio = 100;
	metadata->item_appearance_rate = nSCBattleItemSwitchNone;
	metadata->is_not_teamshadows = TRUE;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		metadata->player_kinds[player] = nFTPlayerKindNot;
		metadata->fighter_kinds[player] = nFTKindNull;
		metadata->costumes[player] = 0;
		metadata->teams[player] = player;
		metadata->handicaps[player] = 9;
		metadata->levels[player] = 3;
		metadata->shades[player] = 0;
	}
	metadata->player_kinds[0] = nFTPlayerKindMan;
	metadata->fighter_kinds[0] = nFTKindMario;
	metadata->player_kinds[1] = nFTPlayerKindMan;
	metadata->fighter_kinds[1] = nFTKindFox;

#ifdef PORT
	/* A lobby-driven session carries the players' picks and the rules. */
	if (sSYNetPeerLobbyHasPicks != FALSE)
	{
		metadata->fighter_kinds[0] = sSYNetPeerLobbyFighters[0];
		metadata->fighter_kinds[1] = sSYNetPeerLobbyFighters[1];
		metadata->stage_kind = sSYNetPeerLobbyStageKind;
		metadata->game_rules = 0;

		if (sSYNetPeerLobbyStocks != 0)
		{
			metadata->game_rules |= SCBATTLE_GAMERULE_STOCK;
			metadata->stocks = sSYNetPeerLobbyStocks;
		}
		if (sSYNetPeerLobbyTimeMinutes != 0)
		{
			metadata->game_rules |= SCBATTLE_GAMERULE_TIME;
			metadata->time_limit = sSYNetPeerLobbyTimeMinutes;
		}
		if (metadata->game_rules == 0)
		{
			metadata->game_rules = SCBATTLE_GAMERULE_STOCK;
			metadata->stocks = 3;
		}
	}
#endif
}

void syNetPeerApplyBootstrapMetadata(const SYNetInputReplayMetadata *metadata)
{
	sSYNetPeerBootstrapMetadata = *metadata;
	syNetReplayApplyBattleMetadata(&sSYNetPeerBootstrapMetadata);
	syUtilsSetRandomSeed(sSYNetPeerBootstrapMetadata.rng_seed);
#ifdef PORT
	if (sSYNetPeerMenuMode != FALSE)
	{
		/* The menus decide which scene comes next. Keep the host's stage
		 * select setting (the replay path forces it off) and name the stage
		 * for the battle scene that may be starting right now. */
		gSCManagerTransferBattleState.is_stage_select = metadata->is_stage_select;
		gSCManagerSceneData.gkind = metadata->stage_kind;
		/* The default state the replay path copies in asks the character
		 * select to reset every slot. Right at the start of a session: yes,
		 * on both sides. At a battle start: no, so the next character select
		 * finds the tokens on the fighters just played, like the console. */
		gSCManagerTransferBattleState.is_reset_players = (sSYNetPeerSessionPhase == 0) ? TRUE : FALSE;
		sSYNetPeerBootstrapMetadataApplied = TRUE;
		port_log("SSB64 NetPeer: battle state applied host=%d stage=%u seed=%u rules=%u stocks=%u time=%u p0=%u/%u p1=%u/%u\n",
		         sSYNetPeerBootstrapIsHost, metadata->stage_kind, metadata->rng_seed, metadata->game_rules,
		         metadata->stocks, metadata->time_limit, metadata->player_kinds[0], metadata->fighter_kinds[0],
		         metadata->player_kinds[1], metadata->fighter_kinds[1]);
		return;
	}
#endif
	gSCManagerSceneData.scene_prev = nSCKindVSMode;
	gSCManagerSceneData.scene_curr = nSCKindVSBattle;
	sSYNetPeerBootstrapMetadataApplied = TRUE;

#ifdef PORT
	port_log("SSB64 NetPeer: bootstrap metadata applied host=%d stage=%u seed=%u players=%u p0=%u/%u p1=%u/%u\n",
	         sSYNetPeerBootstrapIsHost, sSYNetPeerBootstrapMetadata.stage_kind,
	         sSYNetPeerBootstrapMetadata.rng_seed, sSYNetPeerBootstrapMetadata.player_count,
	         sSYNetPeerBootstrapMetadata.player_kinds[0], sSYNetPeerBootstrapMetadata.fighter_kinds[0],
	         sSYNetPeerBootstrapMetadata.player_kinds[1], sSYNetPeerBootstrapMetadata.fighter_kinds[1]);
#endif
}

#ifdef PORT
void syNetPeerSendBytes(const u8 *buffer, u32 size)
{
	syNetPeerSendToRound(buffer, size, -1);
}

void syNetPeerMakeControlPacket(u8 *buffer, u16 packet_type, u32 session_id)
{
	u8 *cursor = buffer;
	u32 checksum;

	syNetPeerWriteU32(&cursor, SYNETPEER_MAGIC);
	syNetPeerWriteU16(&cursor, SYNETPEER_VERSION);
	syNetPeerWriteU16(&cursor, packet_type);
	syNetPeerWriteU32(&cursor, session_id);
	checksum = syNetPeerChecksumBytes(buffer, cursor - buffer);
	syNetPeerWriteU32(&cursor, checksum);
}
#endif

void syNetPeerSendControlPacket(u16 packet_type)
{
#ifdef PORT
	u8 buffer[SYNETPEER_CONTROL_PACKET_BYTES];

	syNetPeerMakeControlPacket(buffer, packet_type, sSYNetPeerSessionID);
	syNetPeerSendBytes(buffer, SYNETPEER_CONTROL_PACKET_BYTES);
#endif
}

#ifdef PORT
/* to: a member, or -1 = PING to everyone / REMATCH to the round. */
void syNetPeerSendPingPacketTo(u16 packet_type, u32 stamp_ms, s32 to)
{
	u8 buffer[SYNETPEER_PING_PACKET_BYTES];
	u8 *cursor = buffer;
	u32 checksum;

	syNetPeerWriteU32(&cursor, SYNETPEER_MAGIC);
	syNetPeerWriteU16(&cursor, SYNETPEER_VERSION);
	syNetPeerWriteU16(&cursor, packet_type);
	syNetPeerWriteU32(&cursor, sSYNetPeerSessionID);
	syNetPeerWriteU32(&cursor, stamp_ms);
	checksum = syNetPeerChecksumBytes(buffer, cursor - buffer);
	syNetPeerWriteU32(&cursor, checksum);

	if (to >= 0)
	{
		syNetPeerSendToMember(to, buffer, SYNETPEER_PING_PACKET_BYTES);
	}
	else if (packet_type == SYNETPEER_PACKET_PING)
	{
		syNetPeerSendToAll(buffer, SYNETPEER_PING_PACKET_BYTES);
	}
	else syNetPeerSendBytes(buffer, SYNETPEER_PING_PACKET_BYTES);
}

void syNetPeerSendPingPacket(u16 packet_type, u32 stamp_ms)
{
	syNetPeerSendPingPacketTo(packet_type, stamp_ms, -1);
}

void syNetPeerHandlePingPacket(const u8 *buffer, s32 size, s32 member)
{
	const u8 *cursor = buffer;
	u32 magic;
	u16 version;
	u16 packet_type;
	u32 session_id;
	u32 stamp_ms;
	u32 checksum;

	if (size != SYNETPEER_PING_PACKET_BYTES)
	{
		return;
	}
	magic = syNetPeerReadU32(&cursor);
	version = syNetPeerReadU16(&cursor);
	packet_type = syNetPeerReadU16(&cursor);
	session_id = syNetPeerReadU32(&cursor);
	stamp_ms = syNetPeerReadU32(&cursor);
	checksum = syNetPeerReadU32(&cursor);

	/* PING/PONG carry no state: answer them whatever phase either side is
	 * in (a member waiting for the next round has no session id yet). */
	if ((magic != SYNETPEER_MAGIC) || (version != SYNETPEER_VERSION) ||
	    (checksum != syNetPeerChecksumBytes(buffer, SYNETPEER_PING_PACKET_BYTES - 4)))
	{
		sSYNetPeerPacketsDropped++;
		return;
	}
	if (packet_type == SYNETPEER_PACKET_PING)
	{
		syNetPeerSendPingPacketTo(SYNETPEER_PACKET_PONG, stamp_ms, member);
	}
	else if (packet_type == SYNETPEER_PACKET_PONG)
	{
		u32 rtt = port_netsock_now_ms() - stamp_ms;
		SYNetPeerMember *m = &sSYNetPeerMembers[member];

		/* Smooth a little; a single late packet should not bounce the HUD. */
		m->ping_ms = (m->ping_ms == 0) ? rtt : (m->ping_ms * 3 + rtt) / 4;
	}
	else if (session_id != sSYNetPeerSessionID)
	{
		sSYNetPeerPacketsDropped++;
	}
	else if (packet_type == SYNETPEER_PACKET_REMATCH)
	{
		sSYNetPeerRematchPeer = TRUE;
		sSYNetPeerRematchPeerFighter = (u8)stamp_ms;
	}
}

void syNetPeerSendMatchConfigPacket(void)
{
	u8 buffer[SYNETPEER_BOOTSTRAP_PACKET_BYTES];
	u8 *cursor = buffer;
	u32 checksum;

	syNetPeerWriteU32(&cursor, SYNETPEER_MAGIC);
	syNetPeerWriteU16(&cursor, SYNETPEER_VERSION);
	syNetPeerWriteU16(&cursor, SYNETPEER_PACKET_MATCH_CONFIG);
	syNetPeerWriteU32(&cursor, sSYNetPeerSessionID);
	syNetPeerWriteMetadata(&cursor, &sSYNetPeerBootstrapMetadata);
	syNetPeerWriteU32(&cursor, gSCManagerBackupData.unlock_mask);
	syNetPeerWriteU32(&cursor, gSCManagerBackupData.fighter_mask);
	checksum = syNetPeerChecksumBytes(buffer, cursor - buffer);
	syNetPeerWriteU32(&cursor, checksum);
	syNetPeerSendBytes(buffer, SYNETPEER_BOOTSTRAP_PACKET_BYTES);
}

#ifdef PORT
void syNetPeerHostDropMember(s32 member, const char *reason);
void syNetPeerGuestHostGone(const char *reason);
#endif

void syNetPeerHandleControlPacket(const u8 *buffer, s32 size, s32 member)
{
	const u8 *cursor = buffer;
	u32 magic;
	u32 session_id;
	u32 checksum;
	u32 expected_checksum;
	u16 version;
	u16 packet_type;

	if (size != SYNETPEER_CONTROL_PACKET_BYTES)
	{
		return;
	}
	expected_checksum = syNetPeerChecksumBytes(buffer, SYNETPEER_CONTROL_PACKET_BYTES - 4);

	magic = syNetPeerReadU32(&cursor);
	version = syNetPeerReadU16(&cursor);
	packet_type = syNetPeerReadU16(&cursor);
	session_id = syNetPeerReadU32(&cursor);
	checksum = syNetPeerReadU32(&cursor);

	if ((magic != SYNETPEER_MAGIC) || (version != SYNETPEER_VERSION) || (checksum != expected_checksum))
	{
		sSYNetPeerPacketsDropped++;
		return;
	}
#ifdef PORT
	if (packet_type == SYNETPEER_PACKET_BYE)
	{
		if (sSYNetPeerIsHub != FALSE)
		{
			syNetPeerHostDropMember(member, "left");
		}
		else syNetPeerGuestHostGone("the host left");

		return;
	}
	if (packet_type == SYNETPEER_PACKET_ROUND_START)
	{
		if ((sSYNetPeerIsHub == FALSE) && (sSYNetPeerRoundState == 1) && (session_id == sSYNetPeerRound.session_id))
		{
			sSYNetPeerRoundApplyPending = TRUE;
		}
		return;
	}
#endif
	if (session_id != sSYNetPeerSessionID)
	{
		sSYNetPeerPacketsDropped++;
		return;
	}
	if (packet_type == SYNETPEER_PACKET_READY)
	{
#ifdef PORT
		sSYNetPeerMembers[member].is_ready = TRUE;
#endif
		sSYNetPeerBootstrapPeerReady = TRUE;
	}
	else if (packet_type == SYNETPEER_PACKET_START)
	{
		sSYNetPeerBootstrapStartReceived = TRUE;
	}
	else if (packet_type == SYNETPEER_PACKET_BATTLE_READY)
	{
#ifdef PORT
		sSYNetPeerMembers[member].is_battle_ready = TRUE;
#endif
		if (sSYNetPeerBattlePeerReady == FALSE)
		{
			port_log("SSB64 NetPeer: received BATTLE_READY role=%s tick=%u local=%d member=%d\n",
			         (sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client",
			         syNetInputGetTick(), sSYNetPeerLocalPlayer, member);
		}
		sSYNetPeerBattlePeerReady = TRUE;
	}
	else if (packet_type == SYNETPEER_PACKET_BATTLE_START)
	{
#ifdef PORT
		sSYNetPeerMembers[member].is_battle_start = TRUE;
#endif
		if (sSYNetPeerBattleStartReceived == FALSE)
		{
			port_log("SSB64 NetPeer: received BATTLE_START role=%s tick=%u local=%d member=%d\n",
			         (sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client",
			         syNetInputGetTick(), sSYNetPeerLocalPlayer, member);
		}
		sSYNetPeerBattleStartReceived = TRUE;
	}
}

void syNetPeerHandleMatchConfigPacket(const u8 *buffer, s32 size)
{
	const u8 *cursor = buffer;
	SYNetInputReplayMetadata metadata;
	u32 magic;
	u32 session_id;
	u32 unlock_mask;
	u32 fighter_mask;
	u32 checksum;
	u32 expected_checksum;
	u16 version;
	u16 packet_type;

	if (size != SYNETPEER_BOOTSTRAP_PACKET_BYTES)
	{
		return;
	}
	expected_checksum = syNetPeerChecksumBytes(buffer, SYNETPEER_BOOTSTRAP_PACKET_BYTES - 4);

	magic = syNetPeerReadU32(&cursor);
	version = syNetPeerReadU16(&cursor);
	packet_type = syNetPeerReadU16(&cursor);
	session_id = syNetPeerReadU32(&cursor);
	syNetPeerReadMetadata(&cursor, &metadata);
	unlock_mask = syNetPeerReadU32(&cursor);
	fighter_mask = syNetPeerReadU32(&cursor);
	checksum = syNetPeerReadU32(&cursor);

	if ((magic != SYNETPEER_MAGIC) || (version != SYNETPEER_VERSION) ||
		(packet_type != SYNETPEER_PACKET_MATCH_CONFIG) ||
		(session_id != sSYNetPeerSessionID) || (checksum != expected_checksum) ||
		(syNetPeerCheckMetadata(&metadata) == FALSE))
	{
		sSYNetPeerPacketsDropped++;
		return;
	}
	if (sSYNetPeerBootstrapIsHost == FALSE)
	{
#ifdef PORT
		if (sSYNetPeerMenuMode != FALSE)
		{
			if (sSYNetPeerMasksOverridden == FALSE)
			{
				sSYNetPeerSavedUnlockMask = gSCManagerBackupData.unlock_mask;
				sSYNetPeerSavedFighterMask = gSCManagerBackupData.fighter_mask;
				sSYNetPeerMasksOverridden = TRUE;
			}
			gSCManagerBackupData.unlock_mask = (u8)unlock_mask;
			gSCManagerBackupData.fighter_mask = (u16)fighter_mask;
		}
#endif
		syNetPeerApplyBootstrapMetadata(&metadata);
	}
}

#ifdef PORT
/* ---- ROUND_CONFIG ---- one per member (it names who you are). */
void syNetPeerSendRoundConfigTo(s32 member)
{
	u8 buffer[SYNETPEER_ROUND_PACKET_BYTES];
	u8 *cursor = buffer;
	SYNetPeerRound *r = &sSYNetPeerRound;
	u32 checksum;
	s32 i;
	s32 c;

	syNetPeerWriteU32(&cursor, SYNETPEER_MAGIC);
	syNetPeerWriteU16(&cursor, SYNETPEER_VERSION);
	syNetPeerWriteU16(&cursor, SYNETPEER_PACKET_ROUND_CONFIG);
	syNetPeerWriteU32(&cursor, r->session_id);
	syNetPeerWriteU32(&cursor, r->session_base);
	syNetPeerWriteU32(&cursor, r->session_phase);
	syNetPeerWriteU8(&cursor, (u8)member);
	syNetPeerWriteU8(&cursor, r->mode);
	syNetPeerWriteU8(&cursor, r->flags);
	syNetPeerWriteU8(&cursor, r->scene);
	syNetPeerWriteU8(&cursor, r->scene_prev);
	syNetPeerWriteU32(&cursor, r->seed);

	for (i = 0; i < SYNETPEER_MAX_MEMBERS; i++)
	{
		syNetPeerWriteU8(&cursor, r->used[i]);
		syNetPeerWriteU8(&cursor, r->in_round[i]);
		syNetPeerWriteU8(&cursor, r->slot[i]);
		syNetPeerWriteU8(&cursor, r->queue_pos[i]);

		for (c = 0; c < SYNETPEER_CODE_BYTES; c++)
		{
			syNetPeerWriteU8(&cursor, (u8)r->code[i][c]);
		}
	}
	syNetPeerWriteMetadata(&cursor, &r->metadata);
	syNetPeerWriteU32(&cursor, r->unlock_mask);
	syNetPeerWriteU32(&cursor, r->fighter_mask);
	checksum = syNetPeerChecksumBytes(buffer, cursor - buffer);
	syNetPeerWriteU32(&cursor, checksum);
	syNetPeerSendToMember(member, buffer, SYNETPEER_ROUND_PACKET_BYTES);
}

void syNetPeerSendRoundReady(u32 session_id)
{
	u8 buffer[SYNETPEER_CONTROL_PACKET_BYTES];

	syNetPeerMakeControlPacket(buffer, SYNETPEER_PACKET_READY, session_id);
	syNetPeerSendToMember(0, buffer, SYNETPEER_CONTROL_PACKET_BYTES);
}

/* Guest. A newer round than anything we have: keep it and say READY (again
 * for every copy, in case the last READY was lost). ROUND_START applies it. */
void syNetPeerHandleRoundConfigPacket(const u8 *buffer, s32 size, s32 member)
{
	const u8 *cursor = buffer;
	SYNetPeerRound r;
	u32 magic;
	u16 version;
	u16 packet_type;
	u32 checksum;
	s32 i;
	s32 c;

	if ((sSYNetPeerIsHub != FALSE) || (member != 0))
	{
		return;
	}
	magic = syNetPeerReadU32(&cursor);
	version = syNetPeerReadU16(&cursor);
	packet_type = syNetPeerReadU16(&cursor);
	r.session_id = syNetPeerReadU32(&cursor);
	r.session_base = syNetPeerReadU32(&cursor);
	r.session_phase = syNetPeerReadU32(&cursor);
	r.your_member = syNetPeerReadU8(&cursor);
	r.mode = syNetPeerReadU8(&cursor);
	r.flags = syNetPeerReadU8(&cursor);
	r.scene = syNetPeerReadU8(&cursor);
	r.scene_prev = syNetPeerReadU8(&cursor);
	r.seed = syNetPeerReadU32(&cursor);

	for (i = 0; i < SYNETPEER_MAX_MEMBERS; i++)
	{
		r.used[i] = syNetPeerReadU8(&cursor);
		r.in_round[i] = syNetPeerReadU8(&cursor);
		r.slot[i] = syNetPeerReadU8(&cursor);
		r.queue_pos[i] = syNetPeerReadU8(&cursor);

		for (c = 0; c < SYNETPEER_CODE_BYTES; c++)
		{
			r.code[i][c] = (char)syNetPeerReadU8(&cursor);
		}
		r.code[i][SYNETPEER_CODE_BYTES - 1] = '\0';
	}
	syNetPeerReadMetadata(&cursor, &r.metadata);
	r.unlock_mask = syNetPeerReadU32(&cursor);
	r.fighter_mask = syNetPeerReadU32(&cursor);
	checksum = syNetPeerReadU32(&cursor);

	if ((magic != SYNETPEER_MAGIC) || (version != SYNETPEER_VERSION) ||
	    (packet_type != SYNETPEER_PACKET_ROUND_CONFIG) || (r.your_member == 0) ||
	    (r.your_member >= SYNETPEER_MAX_MEMBERS) || (syNetPeerCheckMetadata(&r.metadata) == FALSE) ||
	    (checksum != syNetPeerChecksumBytes(buffer, SYNETPEER_ROUND_PACKET_BYTES - 4)))
	{
		sSYNetPeerPacketsDropped++;
		return;
	}
	if ((sSYNetPeerRoundState == 1) && (r.session_id == sSYNetPeerRound.session_id))
	{
		syNetPeerSendRoundReady(r.session_id);
		return;
	}
	/* Older than the round we are in (or hold): a stale copy. */
	if ((r.session_id <= sSYNetPeerSessionID) && (sSYNetPeerRoundsApplied != 0))
	{
		return;
	}
	if ((sSYNetPeerRoundState == 1) && (r.session_id < sSYNetPeerRound.session_id))
	{
		return;
	}
	sSYNetPeerRound = r;
	sSYNetPeerRoundState = 1;
	sSYNetPeerRoundApplyPending = FALSE;
	port_log("SSB64 NetPeer: round config session=%u member=%u slot=%d scene=%u mode=%u flags=%u\n", r.session_id,
	         r.your_member, (r.slot[r.your_member] == SYNETPEER_SLOT_NONE) ? -1 : (s32)r.slot[r.your_member], r.scene,
	         r.mode, r.flags);
	syNetPeerSendRoundReady(r.session_id);
}

void syNetPeerHandleInputPacket(const u8 *buffer, s32 size, s32 member);
#endif

/* Every datagram from a member, whatever phase we are in; each kind checks
 * its own session id. */
void syNetPeerHandleDatagram(const u8 *buffer, s32 size, s32 member)
{
	if (size == SYNETPEER_CONTROL_PACKET_BYTES)
	{
		syNetPeerHandleControlPacket(buffer, size, member);
	}
#ifdef PORT
	else if (size == SYNETPEER_PING_PACKET_BYTES)
	{
		syNetPeerHandlePingPacket(buffer, size, member);
	}
	else if (size == SYNETPEER_PACKET_BYTES)
	{
		syNetPeerHandleInputPacket(buffer, size, member);
	}
	else if (size == SYNETPEER_ROUND_PACKET_BYTES)
	{
		syNetPeerHandleRoundConfigPacket(buffer, size, member);
	}
#endif
	else if (size == SYNETPEER_BOOTSTRAP_PACKET_BYTES)
	{
		syNetPeerHandleMatchConfigPacket(buffer, size);
	}
	else sSYNetPeerPacketsDropped++;
}

void syNetPeerReceiveBootstrapPackets(void)
{
#ifdef PORT
	syNetPeerReceiveAll();
#endif
}

/* Host: all of the round's guests have answered READY. */
sb32 syNetPeerCheckBootstrapPeersReady(void)
{
#ifdef PORT
	return syNetPeerCheckRoundMembers(0, FALSE);
#else
	return sSYNetPeerBootstrapPeerReady;
#endif
}

/* Blocking exchange of the match config. TRUE once both sides hold it (host:
 * READY seen and START sent; client: MATCH_CONFIG applied and START seen). */
sb32 syNetPeerRunBootstrap(void)
{
	s32 i;

	if ((sSYNetPeerBootstrapIsEnabled == FALSE) || (sSYNetPeerIsConfigured == FALSE))
	{
		return FALSE;
	}
	if (syNetPeerOpenSocket() == FALSE)
	{
		return FALSE;
	}
	sSYNetPeerIsActive = TRUE;

	if (sSYNetPeerBootstrapIsHost != FALSE)
	{
		syNetPeerMakeBootstrapMetadata(&sSYNetPeerBootstrapMetadata);
		syNetPeerApplyBootstrapMetadata(&sSYNetPeerBootstrapMetadata);

		for (i = 0; i < SYNETPEER_BOOTSTRAP_RETRY_COUNT; i++)
		{
			syNetPeerSendMatchConfigPacket();
			syNetPeerReceiveBootstrapPackets();

			if (syNetPeerCheckBootstrapPeersReady() != FALSE)
			{
				break;
			}
			syNetPeerSleepBootstrapRetry();
		}
		if (syNetPeerCheckBootstrapPeersReady() == FALSE)
		{
			port_log("SSB64 NetPeer: bootstrap host timed out waiting for READY\n");
			return FALSE;
		}
		for (i = 0; i < 30; i++)
		{
			syNetPeerSendControlPacket(SYNETPEER_PACKET_START);
			syNetPeerSleepBootstrapRetry();
		}
		port_log("SSB64 NetPeer: bootstrap host sent START stage=%u seed=%u\n",
		         sSYNetPeerBootstrapMetadata.stage_kind, sSYNetPeerBootstrapMetadata.rng_seed);
	}
	else
	{
		for (i = 0; i < SYNETPEER_BOOTSTRAP_RETRY_COUNT; i++)
		{
			syNetPeerReceiveBootstrapPackets();

			if (sSYNetPeerBootstrapMetadataApplied != FALSE)
			{
				syNetPeerSendControlPacket(SYNETPEER_PACKET_READY);
				break;
			}
#ifdef PORT
			/* The host gave up on this battle and started a new round. */
			if (sSYNetPeerRoundApplyPending != FALSE)
			{
				return FALSE;
			}
#endif
			syNetPeerSleepBootstrapRetry();
		}
		if (sSYNetPeerBootstrapMetadataApplied == FALSE)
		{
			port_log("SSB64 NetPeer: bootstrap client timed out waiting for MATCH_CONFIG\n");
			return FALSE;
		}
		for (i = 0; i < SYNETPEER_BOOTSTRAP_RETRY_COUNT; i++)
		{
			syNetPeerSendControlPacket(SYNETPEER_PACKET_READY);
			syNetPeerReceiveBootstrapPackets();

			if (sSYNetPeerBootstrapStartReceived != FALSE)
			{
				break;
			}
#ifdef PORT
			if (sSYNetPeerRoundApplyPending != FALSE)
			{
				return FALSE;
			}
#endif
			syNetPeerSleepBootstrapRetry();
		}
		if (sSYNetPeerBootstrapStartReceived == FALSE)
		{
			port_log("SSB64 NetPeer: bootstrap client timed out waiting for START\n");
			return FALSE;
		}
		port_log("SSB64 NetPeer: bootstrap client received START stage=%u seed=%u\n",
		         sSYNetPeerBootstrapMetadata.stage_kind, sSYNetPeerBootstrapMetadata.rng_seed);
	}
	return TRUE;
}

/* A new tick space: the next menu phase or the next battle of an in-menu
 * session. Both sides call this at the same point of the same flow. */
void syNetPeerResetPhaseState(void)
{
	s32 i;

	sSYNetPeerSessionID = sSYNetPeerSessionBase + sSYNetPeerSessionPhase;
	sSYNetPeerBootstrapMetadataApplied = FALSE;
	sSYNetPeerBootstrapPeerReady = FALSE;
	sSYNetPeerBootstrapStartReceived = FALSE;
	sSYNetPeerHighestRemoteTick = 0;
	sSYNetPeerRecvSeqInitialized = FALSE;
	sSYNetPeerLastPacketTicksValid = FALSE;
	sSYNetPeerRemoteAdvantageValid = FALSE;
	sSYNetPeerSendSeq = 0;
	sSYNetPeerPingFrames = 0;

	for (i = 0; i < SYNETPEER_MAX_MEMBERS; i++)
	{
		sSYNetPeerMembers[i].is_ready = FALSE;
		sSYNetPeerMembers[i].is_battle_ready = FALSE;
		sSYNetPeerMembers[i].is_battle_start = FALSE;
	}
	for (i = 0; i < MAXCONTROLLERS; i++)
	{
		sSYNetPeerSlotHighestTick[i] = 0;
		sSYNetPeerSlotHasTick[i] = FALSE;
		sSYNetPeerSlotAdvantageValid[i] = FALSE;
	}
}

void syNetPeerBeginPhase(void)
{
	sSYNetPeerSessionPhase++;
	syNetPeerResetPhaseState();
}

/* Frames of this session that arrived while we were still in the last one
 * go in now, after the phase's netinput reset. */
void syNetPeerApplyEarlyFrames(void)
{
	s32 slot;
	s32 i;

	if (sSYNetPeerEarlySession != sSYNetPeerSessionID)
	{
		return;
	}
	for (slot = 0; slot < MAXCONTROLLERS; slot++)
	{
		for (i = 0; i < SYNETPEER_EARLY_FRAMES; i++)
		{
			SYNetPeerPacketFrame *f = &sSYNetPeerEarlyFrames[slot][i];

			if ((sSYNetPeerEarlyValid[slot][i] == FALSE) || !(sSYNetPeerRemoteMask & (1 << slot)))
			{
				continue;
			}
			syNetInputSetRemoteInput(slot, f->tick, f->buttons, f->stick_x, f->stick_y);

			if ((sSYNetPeerSlotHasTick[slot] == FALSE) || (f->tick > sSYNetPeerSlotHighestTick[slot]))
			{
				sSYNetPeerSlotHighestTick[slot] = f->tick;
				sSYNetPeerSlotHasTick[slot] = TRUE;
			}
		}
	}
	sSYNetPeerEarlySession = 0;
	memset(sSYNetPeerEarlyValid, 0, sizeof(sSYNetPeerEarlyValid));
}

void syNetPeerStashEarlyFrames(u32 session_id, s32 slot, const SYNetPeerPacketFrame *frames, s32 frame_count)
{
	s32 i;

	if (sSYNetPeerEarlySession != session_id)
	{
		memset(sSYNetPeerEarlyValid, 0, sizeof(sSYNetPeerEarlyValid));
		sSYNetPeerEarlySession = session_id;
	}
	for (i = 0; i < frame_count; i++)
	{
		u32 index = frames[i].tick % SYNETPEER_EARLY_FRAMES;

		sSYNetPeerEarlyFrames[slot][index] = frames[i];
		sSYNetPeerEarlyValid[slot][index] = TRUE;
	}
}

sb32 syNetPeerCheckMenuMode(void)
{
	return sSYNetPeerMenuMode;
}

/* PING every half second; the HUD shows the round trip. The battle calls
 * this from syNetPeerUpdate, the menus from sys/netmenu.c. */
void syNetPeerSendPeriodicPing(void)
{
	if (++sSYNetPeerPingFrames >= SYNETPEER_PING_INTERVAL_FRAMES)
	{
		sSYNetPeerPingFrames = 0;
		syNetPeerSendPingPacket(SYNETPEER_PACKET_PING, port_netsock_now_ms());
	}
}
#endif

#ifdef PORT
/* Everything a session needs reset to its "not configured" state, shared by
 * the environment-driven debug path and the lobby path. */
void syNetPeerResetConfig(void)
{
	s32 i;

	sSYNetPeerIsEnabled = FALSE;
	sSYNetPeerIsConfigured = FALSE;
	sSYNetPeerLocalPlayer = 0;
	sSYNetPeerRemotePlayer = 1;
	sSYNetPeerInputDelay = SYNETPEER_DEFAULT_INPUT_DELAY;
	sSYNetPeerSessionID = SYNETPEER_DEFAULT_SESSION_ID;
	sSYNetPeerBootstrapIsEnabled = FALSE;
	sSYNetPeerBootstrapIsHost = FALSE;
	sSYNetPeerBootstrapMetadataApplied = FALSE;
	sSYNetPeerBootstrapPeerReady = FALSE;
	sSYNetPeerBootstrapStartReceived = FALSE;
	sSYNetPeerBootstrapSeed = SYNETPEER_DEFAULT_BOOTSTRAP_SEED;
	sSYNetPeerBattleBarrierEnabled = FALSE;
	sSYNetPeerBattleLocalReady = FALSE;
	sSYNetPeerBattlePeerReady = FALSE;
	sSYNetPeerBattleStartSent = FALSE;
	sSYNetPeerBattleStartReceived = FALSE;
	sSYNetPeerBattleBarrierReleased = TRUE;
	sSYNetPeerBattleBarrierWaitFrames = 0;
	sSYNetPeerBattleStartRepeatFrames = 0;
	sSYNetPeerExecutionHoldFrames = 0;
	sSYNetPeerExecutionBeginLogged = FALSE;
	sSYNetPeerLobbyActive = FALSE;
	sSYNetPeerLobbySession = FALSE;
	sSYNetPeerLobbyHasPicks = FALSE;
	sSYNetPeerPostMatch = FALSE;
	sSYNetPeerRematchLocal = FALSE;
	sSYNetPeerRematchPeer = FALSE;
	sSYNetPeerMenuMode = FALSE;
	sSYNetPeerSessionBase = SYNETPEER_DEFAULT_SESSION_ID;
	sSYNetPeerSessionPhase = 0;
	syNetMenuStop();

	memset(sSYNetPeerMembers, 0, sizeof(sSYNetPeerMembers));

	for (i = 0; i < SYNETPEER_MAX_MEMBERS; i++)
	{
		sSYNetPeerMembers[i].slot = -1;
		sSYNetPeerMembers[i].queue_pos = -1;
	}
	for (i = 0; i < MAXCONTROLLERS; i++)
	{
		sSYNetPeerSlotOwner[i] = -1;
	}
	sSYNetPeerMyMember = 0;
	sSYNetPeerIsHub = FALSE;
	sSYNetPeerRemoteMask = 0;
	sSYNetPeerBattleRunning = FALSE;
	sSYNetPeerGroupMode = SYNETPEER_MODE_ROTATION;
	sSYNetPeerRoundFlags = 0;
	sSYNetPeerRotValid = FALSE;
	sSYNetPeerRotKing = -1;
	sSYNetPeerQueueCount = 0;
	sSYNetPeerRoundState = 0;
	sSYNetPeerRoundApplyPending = FALSE;
	sSYNetPeerRoundAwaiting = FALSE;
	sSYNetPeerRoundFailed = FALSE;
	sSYNetPeerRoundsApplied = 0;
	sSYNetPeerRoundStartRepeat = 0;
	sSYNetPeerEarlySession = 0;
	memset(sSYNetPeerEarlyValid, 0, sizeof(sSYNetPeerEarlyValid));
	port_netplay_set_hazards_off(-1);

	if (sSYNetPeerMasksOverridden != FALSE)
	{
		gSCManagerBackupData.unlock_mask = sSYNetPeerSavedUnlockMask;
		gSCManagerBackupData.fighter_mask = sSYNetPeerSavedFighterMask;
		sSYNetPeerMasksOverridden = FALSE;
	}
}

void syNetPeerCopyCode(char *dst, const char *src)
{
	s32 i;

	for (i = 0; (i < SYNETPEER_CODE_BYTES - 1) && (src != NULL) && (src[i] != '\0'); i++)
	{
		dst[i] = src[i];
	}
	for (; i < SYNETPEER_CODE_BYTES; i++)
	{
		dst[i] = '\0';
	}
}

/* My slot and everyone else's from the member table. */
void syNetPeerApplySlots(void)
{
	s32 i;

	sSYNetPeerRemoteMask = 0;
	sSYNetPeerRemotePlayer = -1;

	for (i = 0; i < MAXCONTROLLERS; i++)
	{
		sSYNetPeerSlotOwner[i] = -1;
	}
	for (i = 0; i < SYNETPEER_MAX_MEMBERS; i++)
	{
		SYNetPeerMember *m = &sSYNetPeerMembers[i];

		if ((m->is_used == FALSE) || (m->is_in_round == FALSE) || (m->slot < 0) || (m->slot >= MAXCONTROLLERS))
		{
			continue;
		}
		sSYNetPeerSlotOwner[m->slot] = i;

		if (i != sSYNetPeerMyMember)
		{
			sSYNetPeerRemoteMask |= 1 << m->slot;

			if ((sSYNetPeerRemotePlayer < 0) || (m->slot < sSYNetPeerRemotePlayer))
			{
				sSYNetPeerRemotePlayer = m->slot;
			}
		}
	}
	sSYNetPeerLocalPlayer = sSYNetPeerMembers[sSYNetPeerMyMember].is_in_round ? sSYNetPeerMembers[sSYNetPeerMyMember].slot : -1;
}

/* Two people and nobody else: the environment-driven debug path and the
 * Netplay page's quick match. Host = member 0. */
void syNetPeerSetupPair(const PortNetAddr *peer, sb32 is_host, s32 local_player, s32 remote_player)
{
	s32 me = (is_host != FALSE) ? 0 : 1;
	s32 them = 1 - me;

	sSYNetPeerIsHub = is_host;
	sSYNetPeerMyMember = me;
	sSYNetPeerMembers[0].is_used = TRUE;
	sSYNetPeerMembers[1].is_used = TRUE;
	sSYNetPeerMembers[0].is_in_round = TRUE;
	sSYNetPeerMembers[1].is_in_round = TRUE;
	sSYNetPeerMembers[me].slot = local_player;
	sSYNetPeerMembers[them].slot = remote_player;
	sSYNetPeerMembers[them].addr = *peer;
	sSYNetPeerMembers[them].last_heard_ms = port_netsock_now_ms();
	sSYNetPeerPeerAddress = *peer;
	syNetPeerApplySlots();
}

/* ---- Lobby-driven sessions (port/netplay/lobby.cpp) ----
 * The lobby has already punched a hole to the peer on `sock`. It hands the
 * socket and the match parameters over here, then calls syNetPeerLobbyStep()
 * once per frame until it reports done. Host = player 0, client = 1.
 *
 * With menu_mode this is a group session of two (see syNetPeerHostConfigure):
 * the picks are ignored and both are dropped into the VS menu in lockstep
 * (sys/netmenu.c) to pick everything there. Without, it is the quick match:
 * MATCH_CONFIG with the picks, then straight into the battle. */
void syNetPeerLobbyConfigure(intptr_t sock, const PortNetAddr *peer, sb32 is_host, u32 delay, u32 session_id,
                             u32 seed, u8 host_fighter, u8 client_fighter, u32 stage_kind, u32 stocks,
                             u32 time_minutes, sb32 menu_mode)
{
	if (menu_mode != FALSE)
	{
		if (is_host != FALSE)
		{
			syNetPeerHostConfigure(sock, delay, session_id, seed);
			syNetPeerHostAddMember(peer, "");
			syNetPeerHostStartSession();
		}
		else syNetPeerGuestConfigure(sock, peer, delay, session_id);

		return;
	}
	if (sSYNetPeerSocket != sock)
	{
		syNetPeerCloseSocket();
	}
	syNetPeerResetConfig();
	sSYNetPeerSessionBase = (session_id != 0) ? session_id : SYNETPEER_DEFAULT_SESSION_ID;
	sSYNetPeerSessionPhase = 0;
	sSYNetPeerLobbySession = TRUE;
	sSYNetPeerLobbyDelay = delay;
	sSYNetPeerLobbyStage = stage_kind;
	sSYNetPeerLobbyStocks = stocks;
	sSYNetPeerLobbyTimeMinutes = time_minutes;

	sSYNetPeerIsEnabled = TRUE;
	sSYNetPeerIsConfigured = TRUE;
	syNetPeerSetupPair(peer, is_host, (is_host != FALSE) ? 0 : 1, (is_host != FALSE) ? 1 : 0);
	sSYNetPeerInputDelay = delay;
	sSYNetPeerSessionID = (session_id != 0) ? session_id : SYNETPEER_DEFAULT_SESSION_ID;
	sSYNetPeerBootstrapIsEnabled = TRUE;
	sSYNetPeerBootstrapIsHost = is_host;
	sSYNetPeerBootstrapSeed = (seed != 0) ? seed : SYNETPEER_DEFAULT_BOOTSTRAP_SEED;
	sSYNetPeerLobbyHasPicks = TRUE;
	sSYNetPeerLobbyFighters[0] = host_fighter;
	sSYNetPeerLobbyFighters[1] = client_fighter;
	sSYNetPeerLobbyStageKind = stage_kind;
	sSYNetPeerSocket = sock;
	sSYNetPeerIsActive = TRUE;
	sSYNetPeerLobbyActive = TRUE;
	/* Predictions are only ever corrected by rollback. */
	syNetStateSetEnabled(TRUE);
	sSYNetPeerLobbyPhase = 0;
	sSYNetPeerLobbyFrames = 0;
	sSYNetPeerLobbyStartRepeat = 0;

	if (is_host != FALSE)
	{
		syNetPeerMakeBootstrapMetadata(&sSYNetPeerBootstrapMetadata);
		syNetPeerApplyBootstrapMetadata(&sSYNetPeerBootstrapMetadata);
	}
	port_log("SSB64 NetPeer: quick match role=%s delay=%u session=%u seed=%u fighters=%u/%u stage=%u stocks=%u time=%u\n",
	         (is_host != FALSE) ? "host" : "client", delay, sSYNetPeerSessionID, sSYNetPeerBootstrapSeed,
	         host_fighter, client_fighter, stage_kind, stocks, time_minutes);
}

/* Shared by both ends of a group session. */
void syNetPeerConfigureGroup(intptr_t sock, sb32 is_host, u32 delay, u32 session_id, u32 seed)
{
	if (sSYNetPeerSocket != sock)
	{
		syNetPeerCloseSocket();
	}
	syNetPeerResetConfig();
	sSYNetPeerSocket = sock;
	sSYNetPeerMenuMode = TRUE;
	sSYNetPeerSessionBase = (session_id != 0) ? session_id : SYNETPEER_DEFAULT_SESSION_ID;
	sSYNetPeerSessionPhase = 0;
	sSYNetPeerSessionID = sSYNetPeerSessionBase;
	sSYNetPeerLobbySession = TRUE;
	sSYNetPeerLobbyDelay = delay;
	sSYNetPeerIsEnabled = TRUE;
	sSYNetPeerIsConfigured = TRUE;
	sSYNetPeerInputDelay = delay;
	sSYNetPeerBootstrapIsEnabled = TRUE;
	sSYNetPeerBootstrapIsHost = is_host;
	sSYNetPeerIsHub = is_host;
	sSYNetPeerBootstrapSeed = (seed != 0) ? seed : SYNETPEER_DEFAULT_BOOTSTRAP_SEED;
	sSYNetPeerLobbyActive = TRUE;
	sSYNetPeerLobbyFrames = 0;
	sSYNetPeerLastPingMs = port_netsock_now_ms();
	syNetStateSetEnabled(TRUE);
}

/* Host of a group session: the lobby collected the guests on `sock`
 * (syNetPeerHostAddMember for each, now or later), and the host's Start
 * calls syNetPeerHostStartSession. */
void syNetPeerHostConfigure(intptr_t sock, u32 delay, u32 session_id, u32 seed)
{
	syNetPeerConfigureGroup(sock, TRUE, delay, session_id, seed);
	sSYNetPeerMyMember = 0;
	sSYNetPeerMembers[0].is_used = TRUE;
	syNetPeerCopyCode(sSYNetPeerMembers[0].code, port_lobby_code());
	port_log("SSB64 NetPeer: hosting a group session delay=%u session=%u seed=%u\n", delay, sSYNetPeerSessionBase,
	         sSYNetPeerBootstrapSeed);
}

sb32 syNetPeerCheckCodesEqual(const char *a, const char *b)
{
	s32 i;

	for (i = 0; i < SYNETPEER_CODE_BYTES; i++)
	{
		if (a[i] != b[i])
		{
			return FALSE;
		}
		if (a[i] == '\0')
		{
			break;
		}
	}
	return TRUE;
}

/* Someone reached the host (before or after Start). Returns their member
 * number, -1 if the session is full. They join at the next round. */
s32 syNetPeerHostAddMember(const PortNetAddr *addr, const char *code)
{
	s32 i;

	if (sSYNetPeerIsHub == FALSE)
	{
		return -1;
	}
	i = syNetPeerFindMember(addr);

	if (i >= 0)
	{
		if ((code != NULL) && (code[0] != '\0'))
		{
			syNetPeerCopyCode(sSYNetPeerMembers[i].code, code);
		}
		return i;
	}
	/* The same person from another address: a friend found by code tries
	 * all of our endpoints (public, LAN, loopback) at once, and whichever
	 * reaches us last is the one they kept. */
	if ((code != NULL) && (code[0] != '\0'))
	{
		for (i = 1; i < SYNETPEER_MAX_MEMBERS; i++)
		{
			if ((sSYNetPeerMembers[i].is_used != FALSE) && (syNetPeerCheckCodesEqual(sSYNetPeerMembers[i].code, code) != FALSE))
			{
				sSYNetPeerMembers[i].addr = *addr;
				return i;
			}
		}
	}
	for (i = 1; i < SYNETPEER_MAX_MEMBERS; i++)
	{
		if (sSYNetPeerMembers[i].is_used == FALSE)
		{
			memset(&sSYNetPeerMembers[i], 0, sizeof(sSYNetPeerMembers[i]));
			sSYNetPeerMembers[i].is_used = TRUE;
			sSYNetPeerMembers[i].addr = *addr;
			sSYNetPeerMembers[i].slot = -1;
			sSYNetPeerMembers[i].queue_pos = -1;
			sSYNetPeerMembers[i].last_heard_ms = port_netsock_now_ms();
			syNetPeerCopyCode(sSYNetPeerMembers[i].code, code);
			port_log("SSB64 NetPeer: member %d joined (%s)\n", i, sSYNetPeerMembers[i].code);
			return i;
		}
	}
	return -1;
}

void syNetPeerHostStartSession(void)
{
	char *minutes_env = getenv("SSB64_NETPLAY_TEST_MINUTES");

	if ((sSYNetPeerIsHub == FALSE) || (sSYNetPeerRoundsApplied != 0) || (sSYNetPeerRoundState != 0))
	{
		return;
	}
	/* Headless tests: short timed matches without touching the VS options. */
	if ((minutes_env != NULL) && (atoi(minutes_env) > 0))
	{
		gSCManagerTransferBattleState.game_rules = SCBATTLE_GAMERULE_TIME;
		gSCManagerTransferBattleState.time_limit = atoi(minutes_env);
	}
	syNetPeerHostStartRound(FALSE, nSCKindVSMode, nSCKindModeSelect);
}

/* Guest of a group session: the lobby reached the host on `sock`. From
 * here we wait for the host's first round. */
void syNetPeerGuestConfigure(intptr_t sock, const PortNetAddr *host, u32 delay, u32 session_id)
{
	syNetPeerConfigureGroup(sock, FALSE, delay, session_id, 0);
	sSYNetPeerMyMember = -1;
	sSYNetPeerMembers[0].is_used = TRUE;
	sSYNetPeerMembers[0].addr = *host;
	sSYNetPeerMembers[0].last_heard_ms = port_netsock_now_ms();
	sSYNetPeerPeerAddress = *host;
	sSYNetPeerLocalPlayer = -1;
	sSYNetPeerRoundAwaiting = TRUE;
	port_log("SSB64 NetPeer: joined a group session, waiting for the host to start\n");
}

/* ---- Rounds ---- */

s32 syNetPeerCountUsedMembers(void)
{
	s32 count = 0;
	s32 i;

	for (i = 0; i < SYNETPEER_MAX_MEMBERS; i++)
	{
		if (sSYNetPeerMembers[i].is_used != FALSE)
		{
			count++;
		}
	}
	return count;
}

void syNetPeerQueueRemove(s32 member)
{
	s32 i;
	s32 j = 0;

	for (i = 0; i < sSYNetPeerQueueCount; i++)
	{
		if (sSYNetPeerQueue[i] != member)
		{
			sSYNetPeerQueue[j++] = sSYNetPeerQueue[i];
		}
	}
	sSYNetPeerQueueCount = j;
}

void syNetPeerQueuePush(s32 member)
{
	syNetPeerQueueRemove(member);
	sSYNetPeerQueue[sSYNetPeerQueueCount++] = member;
}

s32 syNetPeerQueuePop(void)
{
	s32 member;
	s32 i;

	if (sSYNetPeerQueueCount == 0)
	{
		return -1;
	}
	member = sSYNetPeerQueue[0];

	for (i = 1; i < sSYNetPeerQueueCount; i++)
	{
		sSYNetPeerQueue[i - 1] = sSYNetPeerQueue[i];
	}
	sSYNetPeerQueueCount--;

	return member;
}

/* The slot (0 or 1) that won the 1v1 that just ended, from the results
 * screen's places; -1 when it cannot tell (both placed first: the one who
 * was already on stays). */
s32 syNetPeerGetWinnerSlot(void)
{
	s32 p0 = portMNVSResultsGetPlace(0);
	s32 p1 = portMNVSResultsGetPlace(1);

	if ((p0 < 0) || (p1 < 0) || (p0 == p1))
	{
		return -1;
	}
	return (p0 < p1) ? 0 : 1;
}

/* Host: who plays and who watches in the next round. */
void syNetPeerHostBuildRoster(sb32 after_match)
{
	s32 mode = port_lobby_group_mode();
	s32 i;

	if ((mode != SYNETPEER_MODE_ROTATION) && (mode != SYNETPEER_MODE_EVERYONE))
	{
		mode = SYNETPEER_MODE_ROTATION;
	}
	for (i = 0; i < SYNETPEER_MAX_MEMBERS; i++)
	{
		sSYNetPeerMembers[i].slot = -1;
		sSYNetPeerMembers[i].queue_pos = -1;
		sSYNetPeerMembers[i].is_in_round = sSYNetPeerMembers[i].is_used;
	}
	if (mode == SYNETPEER_MODE_EVERYONE)
	{
		s32 slot = 0;

		for (i = 0; i < SYNETPEER_MAX_MEMBERS; i++)
		{
			if (sSYNetPeerMembers[i].is_used != FALSE)
			{
				sSYNetPeerMembers[i].slot = slot++;
			}
		}
		sSYNetPeerRotValid = FALSE;
		sSYNetPeerGroupMode = (u8)mode;
		return;
	}
	if ((sSYNetPeerRotValid == FALSE) || (sSYNetPeerRotMode != mode))
	{
		/* Fresh line-up: the first two in, then everyone else in order. */
		sSYNetPeerRotPlayers[0] = -1;
		sSYNetPeerRotPlayers[1] = -1;
		sSYNetPeerRotKing = -1;
		sSYNetPeerQueueCount = 0;
		after_match = FALSE;
	}
	else if (after_match != FALSE)
	{
		s32 winner = syNetPeerGetWinnerSlot();
		s32 loser;

		if (winner < 0)
		{
			/* A tie: the one who was already on stays. */
			winner = ((sSYNetPeerRotKing >= 0) && (sSYNetPeerRotPlayers[1] == sSYNetPeerRotKing)) ? 1 : 0;
		}
		loser = 1 - winner;
		sSYNetPeerRotKing = sSYNetPeerRotPlayers[winner];

		if ((sSYNetPeerQueueCount > 0) && (sSYNetPeerRotPlayers[loser] >= 0))
		{
			s32 challenger;

			syNetPeerQueuePush(sSYNetPeerRotPlayers[loser]);
			challenger = syNetPeerQueuePop();
			port_log("SSB64 NetPeer: winner stays on: slot %d (member %d) won, member %d steps out, member %d is next\n",
			         winner, sSYNetPeerRotKing, sSYNetPeerRotPlayers[loser], challenger);
			sSYNetPeerRotPlayers[loser] = challenger;
		}
	}
	/* Forget anyone who left, fill empty ports from the front of the line,
	 * and line up anyone new at the back. */
	for (i = 0; i < 2; i++)
	{
		if ((sSYNetPeerRotPlayers[i] >= 0) && (sSYNetPeerMembers[sSYNetPeerRotPlayers[i]].is_used == FALSE))
		{
			sSYNetPeerRotPlayers[i] = -1;
		}
	}
	for (i = sSYNetPeerQueueCount - 1; i >= 0; i--)
	{
		if (sSYNetPeerMembers[sSYNetPeerQueue[i]].is_used == FALSE)
		{
			syNetPeerQueueRemove(sSYNetPeerQueue[i]);
		}
	}
	for (i = 0; i < SYNETPEER_MAX_MEMBERS; i++)
	{
		s32 q;
		sb32 is_listed = ((sSYNetPeerRotPlayers[0] == i) || (sSYNetPeerRotPlayers[1] == i)) ? TRUE : FALSE;

		for (q = 0; q < sSYNetPeerQueueCount; q++)
		{
			if (sSYNetPeerQueue[q] == i)
			{
				is_listed = TRUE;
			}
		}
		if ((sSYNetPeerMembers[i].is_used != FALSE) && (is_listed == FALSE))
		{
			syNetPeerQueuePush(i);
		}
	}
	for (i = 0; i < 2; i++)
	{
		if (sSYNetPeerRotPlayers[i] < 0)
		{
			sSYNetPeerRotPlayers[i] = syNetPeerQueuePop();
		}
	}
	for (i = 0; i < 2; i++)
	{
		if (sSYNetPeerRotPlayers[i] >= 0)
		{
			sSYNetPeerMembers[sSYNetPeerRotPlayers[i]].slot = i;
		}
	}
	for (i = 0; i < sSYNetPeerQueueCount; i++)
	{
		sSYNetPeerMembers[sSYNetPeerQueue[i]].queue_pos = i;
	}
	sSYNetPeerRotValid = TRUE;
	sSYNetPeerRotMode = (u8)mode;
	sSYNetPeerGroupMode = (u8)mode;
}

/* Host: fill sSYNetPeerRound from the member table and the host's VS
 * state. Player ports are human, every other port is closed. */
void syNetPeerHostMakeRound(s32 scene, s32 scene_prev)
{
	SYNetPeerRound *r = &sSYNetPeerRound;
	s32 i;

	r->session_id = sSYNetPeerSessionID;
	r->session_base = sSYNetPeerSessionBase;
	r->session_phase = sSYNetPeerSessionPhase;
	r->your_member = 0;
	r->mode = sSYNetPeerGroupMode;
	r->flags = (port_lobby_preset() != 0) ? SYNETPEER_ROUND_FLAG_PRESET : 0;
	r->scene = (u8)scene;
	r->scene_prev = (u8)scene_prev;
	r->seed = (port_netsock_now_ms() * 2654435761U) | 1U;

	for (i = 0; i < SYNETPEER_MAX_MEMBERS; i++)
	{
		SYNetPeerMember *m = &sSYNetPeerMembers[i];

		r->used[i] = (u8)m->is_used;
		r->in_round[i] = (u8)m->is_in_round;
		r->slot[i] = (m->slot >= 0) ? (u8)m->slot : SYNETPEER_SLOT_NONE;
		r->queue_pos[i] = (m->queue_pos >= 0) ? (u8)m->queue_pos : SYNETPEER_SLOT_NONE;
		syNetPeerCopyCode(r->code[i], m->code);
	}
	syNetReplayCaptureBattleMetadata(&gSCManagerTransferBattleState, &r->metadata);
	r->metadata.rng_seed = r->seed;
	r->metadata.player_count = 0;

	for (i = 0; i < MAXCONTROLLERS; i++)
	{
		r->metadata.player_kinds[i] = nFTPlayerKindNot;
	}
	for (i = 0; i < SYNETPEER_MAX_MEMBERS; i++)
	{
		if ((r->used[i] != FALSE) && (r->slot[i] < MAXCONTROLLERS))
		{
			r->metadata.player_kinds[r->slot[i]] = nFTPlayerKindMan;
			r->metadata.player_count++;
		}
	}
	if (r->flags & SYNETPEER_ROUND_FLAG_PRESET)
	{
		r->metadata.stage_kind = nGRKindPupupu;
		r->metadata.is_stage_select = FALSE;
		r->metadata.item_switch = nSCBattleItemSwitchNone;
		r->metadata.item_appearance_rate = nSCBattleItemSwitchNone;
	}
	r->unlock_mask = gSCManagerBackupData.unlock_mask;
	r->fighter_mask = gSCManagerBackupData.fighter_mask;
}

/* Host: drop the dead, decide the line-up and start telling everyone. The
 * exchange itself runs per frame in syNetPeerServiceRound. */
void syNetPeerHostStartRound(sb32 after_match, s32 scene, s32 scene_prev)
{
	s32 i;

	for (i = 1; i < SYNETPEER_MAX_MEMBERS; i++)
	{
		if ((sSYNetPeerMembers[i].is_used != FALSE) && (syNetPeerMemberSilentMs(i) > SYNETPEER_ROUND_MEMBER_TIMEOUT_MS))
		{
			sSYNetPeerMembers[i].is_used = FALSE;
			port_log("SSB64 NetPeer: member %d (%s) went quiet, dropped\n", i, sSYNetPeerMembers[i].code);
		}
	}
	syNetPeerHostBuildRoster(after_match);
	sSYNetPeerSessionPhase++;
	syNetPeerResetPhaseState();
	syNetPeerHostMakeRound(scene, scene_prev);
	sSYNetPeerRoundState = 1;
	sSYNetPeerRoundFrames = 0;
	sSYNetPeerRoundStartMs = port_netsock_now_ms();
	sSYNetPeerRoundApplyPending = FALSE;

	port_log("SSB64 NetPeer: round start session=%u mode=%u members=%d slots=%d/%d/%d/%d after_match=%d\n",
	         sSYNetPeerSessionID, sSYNetPeerGroupMode, syNetPeerCountUsedMembers(), sSYNetPeerMembers[0].slot,
	         sSYNetPeerMembers[1].slot, sSYNetPeerMembers[2].slot, sSYNetPeerMembers[3].slot, after_match);
}

/* Both ends: the round in sSYNetPeerRound becomes the session's state, and
 * we drop into its scene in lockstep from whatever we were doing. */
void syNetPeerApplyRound(void)
{
	SYNetPeerRound *r = &sSYNetPeerRound;
	s32 i;

	sSYNetPeerSessionBase = r->session_base;
	sSYNetPeerSessionPhase = r->session_phase;
	syNetPeerResetPhaseState();

	if (sSYNetPeerIsHub == FALSE)
	{
		sSYNetPeerMyMember = r->your_member;

		for (i = 0; i < SYNETPEER_MAX_MEMBERS; i++)
		{
			SYNetPeerMember *m = &sSYNetPeerMembers[i];

			m->is_used = (i == 0) ? TRUE : r->used[i];
			m->is_in_round = r->in_round[i];
			m->slot = (r->slot[i] == SYNETPEER_SLOT_NONE) ? -1 : r->slot[i];
			m->queue_pos = (r->queue_pos[i] == SYNETPEER_SLOT_NONE) ? -1 : r->queue_pos[i];
			syNetPeerCopyCode(m->code, r->code[i]);
		}
		if (sSYNetPeerMasksOverridden == FALSE)
		{
			sSYNetPeerSavedUnlockMask = gSCManagerBackupData.unlock_mask;
			sSYNetPeerSavedFighterMask = gSCManagerBackupData.fighter_mask;
			sSYNetPeerMasksOverridden = TRUE;
		}
		gSCManagerBackupData.unlock_mask = (u8)r->unlock_mask;
		gSCManagerBackupData.fighter_mask = (u16)r->fighter_mask;
	}
	sSYNetPeerGroupMode = r->mode;
	sSYNetPeerRoundFlags = r->flags;
	syNetPeerApplySlots();
	port_netplay_set_hazards_off((r->flags & SYNETPEER_ROUND_FLAG_PRESET) ? 1 : 0);

	/* A battle we are leaving early (a player dropped mid-match): its
	 * rollback session ends here, and its scene's own stop is a no-op. */
	if (sSYNetPeerBattleRunning != FALSE)
	{
		syNetStateStopVSSession();
		sSYNetPeerBattleRunning = FALSE;
	}
	sSYNetPeerMenuMode = TRUE;
	sSYNetPeerIsActive = TRUE;
	sSYNetPeerLobbyActive = FALSE;
	sSYNetPeerPostMatch = FALSE;
	sSYNetPeerBootstrapSeed = r->seed;
	syNetPeerApplyBootstrapMetadata(&r->metadata);
	gSCManagerTransferBattleState.is_stage_select = r->metadata.is_stage_select;
	/* Who plays which port may have changed: every round starts the
	 * character select from fresh slots on every machine. */
	gSCManagerTransferBattleState.is_reset_players = TRUE;
	syUtilsSetRandomSeed(r->seed);

	sSYNetPeerRoundState = 0;
	sSYNetPeerRoundApplyPending = FALSE;
	sSYNetPeerRoundAwaiting = FALSE;
	sSYNetPeerRoundsApplied++;

	syNetMenuStop();
	syNetMenuStart(sSYNetPeerLocalPlayer, sSYNetPeerRemoteMask, sSYNetPeerInputDelay, r->scene, r->seed, TRUE);
	syNetPeerApplyEarlyFrames();
	portSCManagerRequestResetTo(r->scene, r->scene_prev);
	port_lobby_notify_round();

	port_log("SSB64 NetPeer: round applied session=%u member=%d slot=%d remote_mask=0x%x scene=%u preset=%d\n",
	         sSYNetPeerSessionID, sSYNetPeerMyMember, sSYNetPeerLocalPlayer, sSYNetPeerRemoteMask, r->scene,
	         (r->flags & SYNETPEER_ROUND_FLAG_PRESET) ? 1 : 0);
}

void syNetPeerSendRoundStart(void)
{
	u8 buffer[SYNETPEER_CONTROL_PACKET_BYTES];

	syNetPeerMakeControlPacket(buffer, SYNETPEER_PACKET_ROUND_START, sSYNetPeerRoundSession);
	syNetPeerSendToRound(buffer, SYNETPEER_CONTROL_PACKET_BYTES, -1);
}

/* Once per frame from wherever the session is (menus, battle, lobby): move
 * a round exchange along. TRUE = a round was applied just now (the caller's
 * scene is being reset; do nothing more with it this frame). */
sb32 syNetPeerServiceRound(void)
{
	if ((sSYNetPeerIsEnabled == FALSE) || (sSYNetPeerMenuMode == FALSE))
	{
		return FALSE;
	}
	syNetPeerReceiveAll();

	if (sSYNetPeerRoundStartRepeat != 0)
	{
		sSYNetPeerRoundStartRepeat--;
		syNetPeerSendRoundStart();
	}
	if (sSYNetPeerIsHub != FALSE)
	{
		s32 i;

		if (sSYNetPeerRoundState != 1)
		{
			return FALSE;
		}
		sSYNetPeerRoundFrames++;

		for (i = 1; i < SYNETPEER_MAX_MEMBERS; i++)
		{
			SYNetPeerMember *m = &sSYNetPeerMembers[i];

			if ((m->is_used == FALSE) || (m->is_ready != FALSE))
			{
				continue;
			}
			if ((port_netsock_now_ms() - sSYNetPeerRoundStartMs) > SYNETPEER_ROUND_MEMBER_TIMEOUT_MS)
			{
				/* Never answered: they are gone. Line up again without them. */
				port_log("SSB64 NetPeer: member %d (%s) did not answer the round, dropped\n", i, m->code);
				m->is_used = FALSE;
				syNetPeerHostStartRound(FALSE, sSYNetPeerRound.scene, sSYNetPeerRound.scene_prev);
				return FALSE;
			}
			if ((sSYNetPeerRoundFrames % SYNETPEER_ROUND_RESEND_FRAMES) == 1)
			{
				syNetPeerSendRoundConfigTo(i);
			}
		}
		if (syNetPeerCheckRoundMembers(0, FALSE) == FALSE)
		{
			return FALSE;
		}
		if (syNetPeerCountUsedMembers() < 2)
		{
			/* Everyone else left: nothing to play. */
			syNetPeerAbortMatch("everyone else left");
			return TRUE;
		}
		sSYNetPeerRoundSession = sSYNetPeerSessionID;
		sSYNetPeerRoundStartRepeat = SYNETPEER_ROUND_START_REPEAT;
		syNetPeerSendRoundStart();
		syNetPeerApplyRound();
		return TRUE;
	}
	/* Guest. */
	if (sSYNetPeerRoundApplyPending != FALSE)
	{
		syNetPeerApplyRound();
		return TRUE;
	}
	if ((sSYNetPeerRoundAwaiting != FALSE) || (sSYNetPeerRoundState == 1))
	{
		u32 now = port_netsock_now_ms();

		if ((now - sSYNetPeerLastPingMs) >= 500)
		{
			sSYNetPeerLastPingMs = now;
			syNetPeerSendPingPacket(SYNETPEER_PACKET_PING, now);
		}
		if (syNetPeerMemberSilentMs(0) > SYNETPEER_HOST_SILENT_MS)
		{
			syNetPeerGuestHostGone("lost the host");
			return TRUE;
		}
	}
	return FALSE;
}

/* sys/netmenu.c: the results screen of a group session just ended (both
 * ends see this on the same lockstep tick). The host lines up the next
 * round; guests wait for it. */
void syNetPeerOnMatchCycleEnd(void)
{
	if ((sSYNetPeerMenuMode == FALSE) || (sSYNetPeerIsEnabled == FALSE))
	{
		return;
	}
	if (sSYNetPeerIsHub != FALSE)
	{
		if (sSYNetPeerRoundState == 0)
		{
			syNetPeerHostStartRound(TRUE, nSCKindPlayersVS, nSCKindVSMode);
		}
	}
	else if (sSYNetPeerRoundState == 0)
	{
		sSYNetPeerRoundAwaiting = TRUE;
	}
}

/* TRUE while a round exchange keeps the menus from ticking. */
sb32 syNetPeerCheckRoundBusy(void)
{
	if (sSYNetPeerMenuMode == FALSE)
	{
		return FALSE;
	}
	if (sSYNetPeerIsHub != FALSE)
	{
		return (sSYNetPeerRoundState == 1) ? TRUE : FALSE;
	}
	return ((sSYNetPeerRoundAwaiting != FALSE) || (sSYNetPeerRoundApplyPending != FALSE)) ? TRUE : FALSE;
}

/* 1 = in the session's menus (enter the battle / VS menu), 0 = still going,
 * -1 = gave up. */
s32 syNetPeerLobbyStep(void)
{
	if (sSYNetPeerMenuMode != FALSE)
	{
		if (sSYNetPeerIsEnabled == FALSE)
		{
			return -1;
		}
		syNetPeerServiceRound();

		if (sSYNetPeerIsEnabled == FALSE)
		{
			return -1;
		}
		return (sSYNetPeerRoundsApplied != 0) ? 1 : 0;
	}
	if (sSYNetPeerLobbyActive == FALSE)
	{
		return -1;
	}
	sSYNetPeerLobbyFrames++;

	if (sSYNetPeerLobbyFrames > 600)
	{
		port_log("SSB64 NetPeer: lobby bootstrap timed out (phase %d)\n", sSYNetPeerLobbyPhase);
		sSYNetPeerLobbyActive = FALSE;
		return -1;
	}
	if (sSYNetPeerBootstrapIsHost != FALSE)
	{
		if (sSYNetPeerLobbyPhase == 0)
		{
			syNetPeerSendMatchConfigPacket();
			syNetPeerReceiveBootstrapPackets();

			if (syNetPeerCheckBootstrapPeersReady() != FALSE)
			{
				sSYNetPeerLobbyPhase = 1;
				sSYNetPeerLobbyStartRepeat = 30;
			}
		}
		else if (sSYNetPeerLobbyPhase == 1)
		{
			syNetPeerSendControlPacket(SYNETPEER_PACKET_START);
			syNetPeerReceiveBootstrapPackets();

			if (--sSYNetPeerLobbyStartRepeat == 0)
			{
				sSYNetPeerLobbyPhase = 2;
			}
		}
	}
	else
	{
		if (sSYNetPeerLobbyPhase == 0)
		{
			syNetPeerReceiveBootstrapPackets();

			if (sSYNetPeerBootstrapMetadataApplied != FALSE)
			{
				syNetPeerSendControlPacket(SYNETPEER_PACKET_READY);
				sSYNetPeerLobbyPhase = 1;
			}
		}
		else if (sSYNetPeerLobbyPhase == 1)
		{
			syNetPeerSendControlPacket(SYNETPEER_PACKET_READY);
			syNetPeerReceiveBootstrapPackets();

			if (sSYNetPeerBootstrapStartReceived != FALSE)
			{
				sSYNetPeerLobbyPhase = 2;
			}
		}
	}
	if (sSYNetPeerLobbyPhase == 2)
	{
		port_log("SSB64 NetPeer: lobby bootstrap complete role=%s stage=%u seed=%u\n",
		         (sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client", sSYNetPeerBootstrapMetadata.stage_kind,
		         sSYNetPeerBootstrapMetadata.rng_seed);
		sSYNetPeerLobbyActive = FALSE;

		/* Both players leave whatever menu they are in for the battle; the
		 * battle state itself was written by syNetPeerApplyBootstrapMetadata. */
		portSCManagerRequestResetTo(nSCKindVSBattle, nSCKindVSMode);
		return 1;
	}
	return 0;
}

void syNetPeerSendBye(void)
{
	u8 buffer[SYNETPEER_CONTROL_PACKET_BYTES];
	s32 i;

	if (sSYNetPeerSocket < 0)
	{
		return;
	}
	syNetPeerMakeControlPacket(buffer, SYNETPEER_PACKET_BYE, sSYNetPeerSessionID);

	for (i = 0; i < SYNETPEER_STOP_REPEAT_PACKETS; i++)
	{
		syNetPeerSendToAll(buffer, SYNETPEER_CONTROL_PACKET_BYTES);
	}
}

void syNetPeerLobbyAbort(void)
{
	syNetPeerSendBye();
	sSYNetPeerLobbyActive = FALSE;
	sSYNetPeerMenuMode = FALSE; /* so the stop below does not re-arm the menus */
	syNetMenuStop();
	syNetPeerStopVSSession();
	syNetPeerCloseSocket();
	syNetPeerResetConfig();
}


/* ---- Rematch (between games of a quick match) ----
 * syNetPeerStopVSSession keeps the socket of a lobby match open and marks
 * the session post-match; the Netplay page offers Rematch / Leave. Each side
 * that wants another game sends REMATCH (with its fighter pick) every half
 * second; when both do, the host re-runs the match config exchange with a
 * fresh seed and syNetPeerLobbyStep takes over again. */
void syNetPeerRematchRequest(u8 my_fighter)
{
	if (sSYNetPeerPostMatch == FALSE)
	{
		return;
	}
	sSYNetPeerRematchLocal = TRUE;
	sSYNetPeerRematchLocalFighter = my_fighter;
	sSYNetPeerRematchFrames = SYNETPEER_REMATCH_INTERVAL_FRAMES; /* send at once */
}

sb32 syNetPeerCheckPostMatch(void)
{
	return sSYNetPeerPostMatch;
}

sb32 syNetPeerCheckPeerWantsRematch(void)
{
	return sSYNetPeerRematchPeer;
}

/* 1 = both agreed and the new match config exchange is running (drive
 * syNetPeerLobbyStep from here on), 0 = waiting, -1 = not in a post-match
 * lobby session. */
s32 syNetPeerRematchStep(void)
{
	if (sSYNetPeerPostMatch == FALSE)
	{
		return -1;
	}
	syNetPeerReceiveAll();

	if (sSYNetPeerRematchLocal != FALSE)
	{
		if (++sSYNetPeerRematchFrames >= SYNETPEER_REMATCH_INTERVAL_FRAMES)
		{
			sSYNetPeerRematchFrames = 0;
			syNetPeerSendPingPacket(SYNETPEER_PACKET_REMATCH, sSYNetPeerRematchLocalFighter);
		}
	}
	if ((sSYNetPeerRematchLocal != FALSE) && (sSYNetPeerRematchPeer != FALSE))
	{
		sb32 is_host = sSYNetPeerBootstrapIsHost;
		u8 host_fighter = (is_host != FALSE) ? sSYNetPeerRematchLocalFighter : sSYNetPeerRematchPeerFighter;
		u8 client_fighter = (is_host != FALSE) ? sSYNetPeerRematchPeerFighter : sSYNetPeerRematchLocalFighter;
		u32 seed = (u32)port_netsock_now_ms() | 1u;
		PortNetAddr peer = sSYNetPeerPeerAddress;

		port_log("SSB64 NetPeer: rematch agreed, fighters=%u/%u\n", host_fighter, client_fighter);
		/* Let the peer see our REMATCH a few more times in case theirs
		 * arrived first and ours was lost; the bootstrap repeats anyway. */
		syNetPeerSendPingPacket(SYNETPEER_PACKET_REMATCH, sSYNetPeerRematchLocalFighter);
		syNetPeerLobbyConfigure(sSYNetPeerSocket, &peer, is_host, sSYNetPeerLobbyDelay,
		                        sSYNetPeerSessionID, seed, host_fighter, client_fighter, sSYNetPeerLobbyStage,
		                        sSYNetPeerLobbyStocks, sSYNetPeerLobbyTimeMinutes, FALSE);
		return 1;
	}
	return 0;
}

/* The match cannot continue (we quit, the host is gone, or nobody is left):
 * tell the others, tear the session down, tell the lobby why, and send the
 * player back to the VS menu. Safe to call from inside the tick: the scene
 * change lands at the next frame boundary like the console-reset path. */
void syNetPeerAbortMatch(const char *reason)
{
	extern void port_lobby_notify_match_over(const char *reason);

	port_log("SSB64 NetPeer: match aborted: %s\n", reason);
	syNetPeerSendBye();
	syNetStateStopVSSession();
	sSYNetPeerLobbySession = FALSE; /* no rematch after a lost connection */
	sSYNetPeerMenuMode = FALSE;
	syNetMenuStop();
	syNetPeerStopVSSession();
	syNetPeerCloseSocket();
	syNetPeerResetConfig();
	port_lobby_notify_match_over(reason);
	portSCManagerRequestResetTo(nSCKindVSMode, nSCKindVSMode);
}

void syNetPeerGuestHostGone(const char *reason)
{
	syNetPeerAbortMatch(reason);
}

/* Host: a guest is gone (BYE, or silent while everyone waited on them). If
 * they were playing, the round cannot go on: line up a new one without
 * them. A spectator just stops getting packets. */
void syNetPeerHostDropMember(s32 member, const char *reason)
{
	SYNetPeerMember *m;
	sb32 was_playing;

	if ((member <= 0) || (member >= SYNETPEER_MAX_MEMBERS) || (sSYNetPeerMembers[member].is_used == FALSE))
	{
		return;
	}
	m = &sSYNetPeerMembers[member];
	was_playing = ((m->is_in_round != FALSE) && (m->slot >= 0)) ? TRUE : FALSE;
	port_log("SSB64 NetPeer: member %d (%s) %s%s\n", member, m->code, reason, (was_playing != FALSE) ? " mid-round" : "");
	m->is_used = FALSE;
	m->is_in_round = FALSE;

	if ((sSYNetPeerMenuMode == FALSE) || (sSYNetPeerRoundsApplied == 0))
	{
		if ((sSYNetPeerMenuMode == FALSE) && (was_playing != FALSE))
		{
			syNetPeerAbortMatch("the other player left");
		}
		return;
	}
	if (syNetPeerCountUsedMembers() < 2)
	{
		syNetPeerAbortMatch("everyone else left");
		return;
	}
	if ((was_playing != FALSE) && (sSYNetPeerRoundState == 0))
	{
		syNetPeerHostStartRound(FALSE, nSCKindPlayersVS, nSCKindVSMode);
	}
}

/* sys/netmenu.c and sys/netstate.c: we have waited ten seconds for someone's
 * input. A guest only gives up if the host itself is gone (otherwise the
 * host is sorting it out). The host drops whoever went quiet and goes on
 * with everyone else. */
void syNetPeerReportStall(const char *reason)
{
	s32 i;
	sb32 dropped = FALSE;

	if ((sSYNetPeerMenuMode == FALSE) || (sSYNetPeerRoundsApplied == 0))
	{
		syNetPeerAbortMatch(reason);
		return;
	}
	if (sSYNetPeerIsHub == FALSE)
	{
		if (syNetPeerMemberSilentMs(0) > SYNETPEER_SILENT_MS)
		{
			syNetPeerAbortMatch(reason);
		}
		return;
	}
	for (i = 1; i < SYNETPEER_MAX_MEMBERS; i++)
	{
		SYNetPeerMember *m = &sSYNetPeerMembers[i];

		if ((m->is_used != FALSE) && (m->is_in_round != FALSE) && (m->slot >= 0) &&
		    (syNetPeerMemberSilentMs(i) > SYNETPEER_SILENT_MS))
		{
			m->slot = -1; /* so the drop below lines up a new round only once */
			m->is_used = FALSE;
			dropped = TRUE;
			port_log("SSB64 NetPeer: member %d (%s) stopped sending, dropped\n", i, m->code);
		}
	}
	if (dropped == FALSE)
	{
		/* Everyone still answers pings, yet the game has not moved for ten
		 * seconds: the players' games no longer agree (one already left
		 * the battle the other is still in). Start over from the
		 * character select with the same roster. */
		if (sSYNetPeerRoundState == 0)
		{
			port_log("SSB64 NetPeer: stuck with everyone connected (%s), restarting at character select\n", reason);
			syNetPeerHostStartRound(FALSE, nSCKindPlayersVS, nSCKindVSMode);
		}
		return;
	}
	if (syNetPeerCountUsedMembers() < 2)
	{
		syNetPeerAbortMatch("everyone else left");
		return;
	}
	if (sSYNetPeerRoundState == 0)
	{
		syNetPeerHostStartRound(FALSE, nSCKindPlayersVS, nSCKindVSMode);
	}
}

/* ---- What the lobby panel shows ---- */
s32 syNetPeerGetMemberInfo(s32 member, const char **code, s32 *slot, s32 *queue_pos, sb32 *is_me)
{
	SYNetPeerMember *m;

	if ((member < 0) || (member >= SYNETPEER_MAX_MEMBERS) || (sSYNetPeerIsEnabled == FALSE) ||
	    (sSYNetPeerMenuMode == FALSE))
	{
		return FALSE;
	}
	m = &sSYNetPeerMembers[member];

	if ((m->is_used == FALSE) && !((sSYNetPeerIsHub == FALSE) && (sSYNetPeerRound.used[member] != FALSE)))
	{
		return FALSE;
	}
	*code = m->code;
	*slot = (m->is_in_round != FALSE) ? m->slot : -1;
	*queue_pos = m->queue_pos;
	*is_me = (member == sSYNetPeerMyMember) ? TRUE : FALSE;

	return TRUE;
}

/* Who plays player slot `slot` this round, as the name part of their
 * connect code ("ADO" for ADO#1148), for the game's own name labels. NULL
 * outside a session, for an empty slot, or for a member without a code.
 * The table comes from the host's round config, so every machine in the
 * session gets the same answer: labels built from it make the same objects
 * everywhere, which keeps the rollback state identical. */
const char* syNetPeerGetSlotName(s32 slot)
{
	static char names[MAXCONTROLLERS][SYNETPEER_CODE_BYTES];
	s32 i;
	s32 c;

	if ((slot < 0) || (slot >= MAXCONTROLLERS) || (sSYNetPeerIsEnabled == FALSE) || (sSYNetPeerMenuMode == FALSE) ||
	    (sSYNetPeerRoundsApplied == 0))
	{
		return NULL;
	}
	for (i = 0; i < SYNETPEER_MAX_MEMBERS; i++)
	{
		SYNetPeerMember *m = &sSYNetPeerMembers[i];

		if ((m->is_used == FALSE) || (m->is_in_round == FALSE) || (m->slot != slot))
		{
			continue;
		}
		for (c = 0; (c < SYNETPEER_CODE_BYTES - 1) && (m->code[c] != '\0') && (m->code[c] != '#'); c++)
		{
			names[slot][c] = m->code[c];
		}
		names[slot][c] = '\0';

		return (c != 0) ? names[slot] : NULL;
	}
	return NULL;
}

sb32 syNetPeerCheckSpectating(void)
{
	return ((sSYNetPeerIsEnabled != FALSE) && (sSYNetPeerMenuMode != FALSE) && (sSYNetPeerRoundsApplied != 0) &&
	        (sSYNetPeerLocalPlayer < 0)) ? TRUE : FALSE;
}

sb32 syNetPeerCheckGroupHost(void)
{
	return ((sSYNetPeerIsEnabled != FALSE) && (sSYNetPeerMenuMode != FALSE) && (sSYNetPeerIsHub != FALSE)) ? TRUE : FALSE;
}
#endif

void syNetPeerInitDebugEnv(void)
{
#ifdef PORT
	char *netplay_env = getenv("SSB64_NETPLAY");
	char *local_player_env;
	char *remote_player_env;
	char *delay_env;
	char *session_env;
	char *bind_env;
	char *peer_env;
	char *bootstrap_env;
	char *bootstrap_host_env;
	char *bootstrap_seed_env;

	syNetPeerResetConfig();

	if ((netplay_env == NULL) || (atoi(netplay_env) == 0))
	{
		return;
	}
	sSYNetPeerIsEnabled = TRUE;

	local_player_env = getenv("SSB64_NETPLAY_LOCAL_PLAYER");
	remote_player_env = getenv("SSB64_NETPLAY_REMOTE_PLAYER");
	delay_env = getenv("SSB64_NETPLAY_DELAY");
	session_env = getenv("SSB64_NETPLAY_SESSION");
	bind_env = getenv("SSB64_NETPLAY_BIND");
	peer_env = getenv("SSB64_NETPLAY_PEER");
	bootstrap_env = getenv("SSB64_NETPLAY_BOOTSTRAP");
	bootstrap_host_env = getenv("SSB64_NETPLAY_HOST");
	bootstrap_seed_env = getenv("SSB64_NETPLAY_SEED");

	if (local_player_env != NULL)
	{
		sSYNetPeerLocalPlayer = atoi(local_player_env);
	}
	if (remote_player_env != NULL)
	{
		sSYNetPeerRemotePlayer = atoi(remote_player_env);
	}
	if (delay_env != NULL)
	{
		s32 delay = atoi(delay_env);

		if (delay >= 0)
		{
			sSYNetPeerInputDelay = delay;
		}
	}
	if (session_env != NULL)
	{
		s32 session_id = atoi(session_env);

		if (session_id > 0)
		{
			sSYNetPeerSessionID = session_id;
		}
	}
	if ((bootstrap_env != NULL) && (atoi(bootstrap_env) != 0))
	{
		sSYNetPeerBootstrapIsEnabled = TRUE;
	}
	if ((bootstrap_host_env != NULL) && (atoi(bootstrap_host_env) != 0))
	{
		sSYNetPeerBootstrapIsHost = TRUE;
	}
	if (bootstrap_seed_env != NULL)
	{
		s32 seed = atoi(bootstrap_seed_env);

		if (seed > 0)
		{
			sSYNetPeerBootstrapSeed = seed;
		}
	}
	if ((sSYNetPeerLocalPlayer < 0) || (sSYNetPeerLocalPlayer >= MAXCONTROLLERS) ||
		(sSYNetPeerRemotePlayer < 0) || (sSYNetPeerRemotePlayer >= MAXCONTROLLERS) ||
		(sSYNetPeerLocalPlayer == sSYNetPeerRemotePlayer))
	{
		port_log("SSB64 NetPeer: invalid players local=%d remote=%d\n",
		         sSYNetPeerLocalPlayer, sSYNetPeerRemotePlayer);
		return;
	}
	if ((syNetPeerParseIPv4Address(bind_env, &sSYNetPeerBindAddress) == FALSE) ||
		(syNetPeerParseIPv4Address(peer_env, &sSYNetPeerPeerAddress) == FALSE))
	{
		port_log("SSB64 NetPeer: invalid bind/peer; expected IPv4 host:port\n");
		return;
	}
	sSYNetPeerIsConfigured = TRUE;
	{
		s32 local_player = sSYNetPeerLocalPlayer;
		s32 remote_player = sSYNetPeerRemotePlayer;

		syNetPeerSetupPair(&sSYNetPeerPeerAddress, sSYNetPeerBootstrapIsHost, local_player, remote_player);
	}
	port_log("SSB64 NetPeer: configured local=%d remote=%d delay=%u session=%u bootstrap=%d host=%d seed=%u bind=%s peer=%s\n",
	         sSYNetPeerLocalPlayer, sSYNetPeerRemotePlayer, sSYNetPeerInputDelay,
	         sSYNetPeerSessionID, sSYNetPeerBootstrapIsEnabled, sSYNetPeerBootstrapIsHost,
	         sSYNetPeerBootstrapSeed, bind_env, peer_env);
	syNetPeerRunBootstrap();
#endif
}

void syNetPeerStartVSSession(void)
{
#ifdef PORT
	if ((sSYNetPeerIsEnabled == FALSE) || (sSYNetPeerIsConfigured == FALSE))
	{
		return;
	}
	if (syNetPeerOpenSocket() == FALSE)
	{
		return;
	}
	if (sSYNetPeerMenuMode != FALSE)
	{
		if (sSYNetPeerIsActive == FALSE)
		{
			return; /* the session ended while the menus were running */
		}
		/* The menus brought both players here; now the host's battle state
		 * (its picks, rules and stage) becomes the one true one, exchanged
		 * before either side builds a fighter. New tick space for the
		 * battle; stale menu packets carry the old session id. */
		syNetMenuStop();
		syNetPeerBeginPhase();

		if (syNetPeerRunBootstrap() == FALSE)
		{
			/* The host may have started a new round instead (a player
			 * dropped): that resets us into it. Otherwise give up. */
			if (sSYNetPeerRoundApplyPending != FALSE)
			{
				syNetPeerServiceRound();
				return;
			}
			syNetPeerReportStall("the other player did not start the match");

			if (sSYNetPeerIsHub != FALSE)
			{
				syNetPeerAbortMatch("the other player did not start the match");
			}
			return;
		}
		sSYNetPeerBattleRunning = TRUE;
	}
	sSYNetPeerHighestRemoteTick = 0;
	sSYNetPeerPacketsSent = 0;
	sSYNetPeerPacketsReceived = 0;
	sSYNetPeerPacketsDropped = 0;
	sSYNetPeerFramesStaged = 0;
	sSYNetPeerLateFrames = 0;
	sSYNetPeerInputChecksum = 2166136261U;
	sSYNetPeerLastLogTick = 0;
	sSYNetPeerSendSeq = 0;
	sSYNetPeerRecvSeqInitialized = FALSE;
	sSYNetPeerRecvSeqHighWater = 0;
	sSYNetPeerSeqGaps = 0;
	sSYNetPeerSeqDuplicates = 0;
	sSYNetPeerSeqOutOfOrder = 0;
	sSYNetPeerLastPeerAckTick = 0;
	sSYNetPeerLastPacketTicksValid = FALSE;
	sSYNetPeerRemoteAdvantage = 0;
	sSYNetPeerRemoteAdvantageValid = FALSE;
	{
		s32 slot;

		for (slot = 0; slot < MAXCONTROLLERS; slot++)
		{
			sSYNetPeerSlotHighestTick[slot] = 0;
			sSYNetPeerSlotHasTick[slot] = FALSE;
			sSYNetPeerSlotAdvantageValid[slot] = FALSE;
		}
	}
	sSYNetPeerPingMs = 0;
	sSYNetPeerPingFrames = 0;
	sSYNetPeerIsActive = TRUE;
	/* Watching keeps the barrier too, released locally once the players'
	 * first input is in (syNetPeerUpdateStartBarrier): the frame that
	 * releases it runs one battle step on tick 0's input before the tick
	 * advances, and a spectator has to take that same step. */
	sSYNetPeerBattleBarrierEnabled = (sSYNetPeerBootstrapIsEnabled != FALSE) ? TRUE : FALSE;
	sSYNetPeerBattleLocalReady = sSYNetPeerBattleBarrierEnabled;
	sSYNetPeerBattlePeerReady = FALSE;
	sSYNetPeerBattleStartSent = FALSE;
	sSYNetPeerBattleStartReceived = FALSE;
	sSYNetPeerBattleBarrierReleased = (sSYNetPeerBattleBarrierEnabled == FALSE) ? TRUE : FALSE;
	sSYNetPeerBattleBarrierWaitFrames = 0;
	sSYNetPeerBattleStartRepeatFrames = 0;
	sSYNetPeerExecutionHoldFrames = 0;
	sSYNetPeerExecutionBeginLogged = (sSYNetPeerBattleBarrierEnabled == FALSE) ? TRUE : FALSE;

	{
		s32 slot;

		/* Every port someone else plays is predicted and corrected by
		 * rollback; ours reads our pad; an empty port reads neutral. */
		for (slot = 0; slot < MAXCONTROLLERS; slot++)
		{
			if (slot == sSYNetPeerLocalPlayer)
			{
				syNetInputSetSlotSource(slot, nSYNetInputSourceLocal);
			}
			else if (sSYNetPeerRemoteMask & (1 << slot))
			{
				syNetInputSetSlotSource(slot, nSYNetInputSourceRemotePredicted);
			}
			else syNetInputSetSlotSource(slot, nSYNetInputSourceSaved);
		}
	}
	syNetInputSetLocalDelay(sSYNetPeerInputDelay);
	/* A guest plays port 2 (or 3, 4) but holds its controller in port 1. */
	syNetInputSetLocalHardwarePort((gSYControllerDeviceStatuses[0] >= 0) ? gSYControllerDeviceStatuses[0] : 0);
	syNetPeerApplyEarlyFrames();

	port_log("SSB64 NetPeer: VS session start role=%s local=%d remote_mask=0x%x delay=%u barrier=%d tick=%u\n",
	         (sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client",
	         sSYNetPeerLocalPlayer, sSYNetPeerRemoteMask, sSYNetPeerInputDelay,
	         sSYNetPeerBattleBarrierEnabled, syNetInputGetTick());
#endif
}

sb32 syNetPeerCheckBattleExecutionReady(void)
{
	if ((sSYNetPeerIsEnabled == FALSE) || (sSYNetPeerBootstrapIsEnabled == FALSE) ||
		(sSYNetPeerBattleBarrierEnabled == FALSE))
	{
		return TRUE;
	}
	return sSYNetPeerBattleBarrierReleased;
}

sb32 syNetPeerCheckStartBarrierReleased(void)
{
	return syNetPeerCheckBattleExecutionReady();
}

void syNetPeerBuildPacket(u8 *buffer, u32 *out_size)
{
	SYNetInputFrame queued_frame;
	SYNetPeerPacketFrame frames[SYNETPEER_MAX_PACKET_FRAMES];
	SYNetPeerPacketFrame zero_frame;
	u8 *cursor = buffer;
	u32 queued_count;
	u32 latest_tick;
	u32 checksum;
	s32 frame_count = 0;
	s32 back;
	s32 i;

	memset(frames, 0, sizeof(frames));
	memset(&zero_frame, 0, sizeof(zero_frame));

	/* Send from the local input queue: each frame is tagged with the tick it
	 * will be applied on (poll tick + input delay), which is the tick the
	 * peer applies it on too. The input delay therefore lives entirely in
	 * netinput; the wire carries application ticks. */
	queued_count = syNetInputGetLocalQueuedCount(sSYNetPeerLocalPlayer);

	if (queued_count == 0)
	{
		*out_size = 0;
		return;
	}
	latest_tick = queued_count - 1;

	for (back = SYNETPEER_MAX_PACKET_FRAMES - 1; back >= 0; back--)
	{
		if ((latest_tick >= (u32)back) &&
			(syNetInputGetLocalQueuedFrame(sSYNetPeerLocalPlayer, latest_tick - back, &queued_frame) != FALSE))
		{
			frames[frame_count].tick = queued_frame.tick;
			frames[frame_count].buttons = queued_frame.buttons;
			frames[frame_count].stick_x = queued_frame.stick_x;
			frames[frame_count].stick_y = queued_frame.stick_y;
			frame_count++;
		}
	}
	checksum = syNetPeerChecksumPacket
	(
		sSYNetPeerSessionID,
		sSYNetPeerHighestRemoteTick,
		sSYNetPeerSendSeq,
		(u8)sSYNetPeerLocalPlayer,
		(u8)frame_count,
		frames
	);

	syNetPeerWriteU32(&cursor, SYNETPEER_MAGIC);
	syNetPeerWriteU16(&cursor, SYNETPEER_VERSION);
	syNetPeerWriteU16(&cursor, (u16)(s16)syNetPeerGetLocalAdvantage());
	syNetPeerWriteU32(&cursor, sSYNetPeerSessionID);
	syNetPeerWriteU32(&cursor, sSYNetPeerHighestRemoteTick);
	syNetPeerWriteU32(&cursor, sSYNetPeerSendSeq);
	syNetPeerWriteU8(&cursor, (u8)sSYNetPeerLocalPlayer);
	syNetPeerWriteU8(&cursor, (u8)frame_count);
	syNetPeerWriteU8(&cursor, (u8)sSYNetPeerLocalPlayer);
	syNetPeerWriteU8(&cursor, SYNETPEER_SLOT_NONE); /* was "remote player"; a group has several */

	for (i = 0; i < SYNETPEER_MAX_PACKET_FRAMES; i++)
	{
		SYNetPeerPacketFrame *frame = (i < frame_count) ? &frames[i] : &zero_frame;

		syNetPeerWriteU32(&cursor, frame->tick);
		syNetPeerWriteU16(&cursor, frame->buttons);
		syNetPeerWriteU8(&cursor, (u8)frame->stick_x);
		syNetPeerWriteU8(&cursor, (u8)frame->stick_y);
	}
	syNetPeerWriteU32(&cursor, checksum);
	*out_size = SYNETPEER_PACKET_BYTES;
}

void syNetPeerSendLocalInput(void)
{
#ifdef PORT
	u8 buffer[SYNETPEER_PACKET_BYTES];
	u32 size;

	/* Watching: there is nothing of ours to send. */
	if ((sSYNetPeerLocalPlayer < 0) || (sSYNetPeerLocalPlayer >= MAXCONTROLLERS))
	{
		return;
	}
	syNetPeerBuildPacket(buffer, &size);

	if (size == 0)
	{
		return;
	}
	syNetPeerSendToRound(buffer, size, -1);
	sSYNetPeerPacketsSent++;
	sSYNetPeerSendSeq++;
#endif
}

/* An input packet from a member. A guest's arrives at the host, which
 * forwards it unchanged to everyone else in the round; at a guest every
 * input packet comes from the host, its own or forwarded. */
void syNetPeerHandleInputPacket(const u8 *buffer, s32 size, s32 member)
{
	const u8 *cursor = buffer;
	SYNetPeerPacketFrame frames[SYNETPEER_MAX_PACKET_FRAMES];
	u32 magic;
	u32 session_id;
	u32 ack_tick;
	u32 packet_seq;
	u32 checksum;
	u32 expected_checksum;
	u16 version;
	s32 remote_advantage;
	u8 player;
	u8 frame_count;
	u8 packet_local_player;
	u32 current_tick = syNetInputGetTick();
	s32 i;

	if (size != SYNETPEER_PACKET_BYTES)
	{
		sSYNetPeerPacketsDropped++;
		return;
	}
	memset(frames, 0, sizeof(frames));

	magic = syNetPeerReadU32(&cursor);
	version = syNetPeerReadU16(&cursor);
	remote_advantage = (s16)syNetPeerReadU16(&cursor);
	session_id = syNetPeerReadU32(&cursor);
	ack_tick = syNetPeerReadU32(&cursor);
	packet_seq = syNetPeerReadU32(&cursor);
	player = syNetPeerReadU8(&cursor);
	frame_count = syNetPeerReadU8(&cursor);
	packet_local_player = syNetPeerReadU8(&cursor);
	(void)syNetPeerReadU8(&cursor);

	if ((magic != SYNETPEER_MAGIC) || (version != SYNETPEER_VERSION) || (player >= MAXCONTROLLERS) ||
		(packet_local_player != player) || (frame_count > SYNETPEER_MAX_PACKET_FRAMES))
	{
		sSYNetPeerPacketsDropped++;
		return;
	}
	for (i = 0; i < SYNETPEER_MAX_PACKET_FRAMES; i++)
	{
		frames[i].tick = syNetPeerReadU32(&cursor);
		frames[i].buttons = syNetPeerReadU16(&cursor);
		frames[i].stick_x = (s8)syNetPeerReadU8(&cursor);
		frames[i].stick_y = (s8)syNetPeerReadU8(&cursor);
	}
	checksum = syNetPeerReadU32(&cursor);
	expected_checksum = syNetPeerChecksumPacket(session_id, ack_tick, packet_seq, player, frame_count, frames);

	if (checksum != expected_checksum)
	{
		sSYNetPeerPacketsDropped++;
		return;
	}
#ifdef PORT
	if (sSYNetPeerIsHub != FALSE)
	{
		/* Only the member playing that port speaks for it. */
		if ((sSYNetPeerMembers[member].is_in_round == FALSE) || (sSYNetPeerMembers[member].slot != (s32)player))
		{
			sSYNetPeerPacketsDropped++;
			return;
		}
		if ((session_id == sSYNetPeerSessionID) || (session_id == sSYNetPeerSessionID + 1))
		{
			syNetPeerSendToRound(buffer, size, member);
		}
	}
	if (session_id == sSYNetPeerSessionID + 1)
	{
		/* They are already in the next phase (results while we finish the
		 * battle): keep it for when we get there. */
		syNetPeerStashEarlyFrames(session_id, player, frames, frame_count);
		return;
	}
#endif
	if ((session_id != sSYNetPeerSessionID) || !(sSYNetPeerRemoteMask & (1 << player)))
	{
		sSYNetPeerPacketsDropped++;
		return;
	}
	sSYNetPeerPacketsReceived++;
	sSYNetPeerSlotAdvantage[player] = remote_advantage;
	sSYNetPeerSlotAdvantageValid[player] = TRUE;

	if (player == sSYNetPeerRemotePlayer)
	{
		if (sSYNetPeerRecvSeqInitialized == FALSE)
		{
			sSYNetPeerRecvSeqHighWater = packet_seq;
			sSYNetPeerRecvSeqInitialized = TRUE;
		}
		else
		{
			if (packet_seq > sSYNetPeerRecvSeqHighWater)
			{
				sSYNetPeerSeqGaps += packet_seq - sSYNetPeerRecvSeqHighWater - 1U;
				sSYNetPeerRecvSeqHighWater = packet_seq;
			}
			else if (packet_seq == sSYNetPeerRecvSeqHighWater)
			{
				sSYNetPeerSeqDuplicates++;
			}
			else
			{
				sSYNetPeerSeqOutOfOrder++;
			}
		}
		sSYNetPeerLastPeerAckTick = ack_tick;
	}
	if (frame_count > 0)
	{
		u32 oldest_tick_bundle = frames[0].tick;
		u32 newest_tick_bundle = frames[0].tick;

		for (i = 1; i < frame_count; i++)
		{
			if (frames[i].tick < oldest_tick_bundle)
			{
				oldest_tick_bundle = frames[i].tick;
			}
			if (frames[i].tick > newest_tick_bundle)
			{
				newest_tick_bundle = frames[i].tick;
			}
		}
		sSYNetPeerLastPacketOldestTick = oldest_tick_bundle;
		sSYNetPeerLastPacketNewestTick = newest_tick_bundle;
		sSYNetPeerLastPacketTicksValid = TRUE;
	}
	else sSYNetPeerLastPacketTicksValid = FALSE;

	for (i = 0; i < frame_count; i++)
	{
		sb32 is_new_remote_tick = ((sSYNetPeerSlotHasTick[player] == FALSE) || (frames[i].tick > sSYNetPeerSlotHighestTick[player])) ? TRUE : FALSE;

		if ((is_new_remote_tick != FALSE) && (frames[i].tick < current_tick))
		{
			sSYNetPeerLateFrames++;
		}
		if (is_new_remote_tick != FALSE)
		{
			sSYNetPeerSlotHighestTick[player] = frames[i].tick;
			sSYNetPeerSlotHasTick[player] = TRUE;
		}
		syNetInputSetRemoteInput
		(
			player,
			frames[i].tick,
			frames[i].buttons,
			frames[i].stick_x,
			frames[i].stick_y
		);
		sSYNetPeerFramesStaged++;
		sSYNetPeerInputChecksum = syNetPeerChecksumAccumulateU32(sSYNetPeerInputChecksum, player);
		sSYNetPeerInputChecksum = syNetPeerChecksumAccumulateFrame(sSYNetPeerInputChecksum, &frames[i]);
	}
	/* The newest tick we hold from every remote port: what the time sync
	 * and the HUD measure against. */
	{
		u32 lowest = 0;
		sb32 any = FALSE;
		s32 slot;

		for (slot = 0; slot < MAXCONTROLLERS; slot++)
		{
			if (!(sSYNetPeerRemoteMask & (1 << slot)))
			{
				continue;
			}
			if (sSYNetPeerSlotHasTick[slot] == FALSE)
			{
				lowest = 0;
				any = TRUE;
				break;
			}
			if ((any == FALSE) || (sSYNetPeerSlotHighestTick[slot] < lowest))
			{
				lowest = sSYNetPeerSlotHighestTick[slot];
				any = TRUE;
			}
		}
		sSYNetPeerHighestRemoteTick = lowest;
	}
}

void syNetPeerReceiveRemoteInput(void)
{
#ifdef PORT
	syNetPeerReceiveAll();
#endif
}

/* Called by netstate before each real tick, ahead of the stall check and the
 * rollback it may run. The battle gate also receives, but only after the
 * tick's simulation, so a packet that came in while the last frame was being
 * drawn would otherwise sit in the socket for a whole tick: its input missed
 * that tick (one more frame predicted, one deeper rollback), and a PING
 * waited a frame for its PONG, which the HUD then showed as ping. */
void syNetPeerReceiveBeforeTick(void)
{
	if (sSYNetPeerIsActive == FALSE)
	{
		return;
	}
	syNetPeerReceiveRemoteInput();
}

#ifdef PORT
void syNetPeerLogNetSyncValidation(u32 tick)
{
	u32 checksums[MAXCONTROLLERS];
	u32 inp_all = 0;
	u32 fighter_hash;
	u32 win_begin = 0;
	u32 win_length = tick;

	if ((sSYNetPeerIsActive == FALSE) || (syNetPeerCheckBattleExecutionReady() == FALSE))
	{
		return;
	}
	if (tick >= SYNETPEER_VALIDATION_INPUT_WINDOW)
	{
		win_begin = tick - SYNETPEER_VALIDATION_INPUT_WINDOW;
		win_length = SYNETPEER_VALIDATION_INPUT_WINDOW;
	}
	syNetInputGetHistoryInputValueChecksumWindow(win_begin, win_length, checksums, &inp_all);

	fighter_hash = syNetSyncHashBattleFighters();

	port_log(
		"SSB64 NetSync: role=%s lp=%d rp=%d tick=%u hist_win=[%u,%u) all=0x%08X p0=0x%08X p1=0x%08X p2=0x%08X p3=0x%08X figh=0x%08X snd_next=%u rcv_hw=%u gap=%u dup=%u ooo=%u puck=%u pko=%u pkn=%u sent=%u recv=%u dropped=%u stg=%u hr=%u late=%u inpchk=0x%08X pkt_valid=%d\n",
		(sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client",
		sSYNetPeerLocalPlayer,
		sSYNetPeerRemotePlayer,
		tick,
		win_begin,
		win_begin + win_length,
		inp_all,
		checksums[0],
		checksums[1],
		checksums[2],
		checksums[3],
		fighter_hash,
		sSYNetPeerSendSeq,
		sSYNetPeerRecvSeqHighWater,
		sSYNetPeerSeqGaps,
		sSYNetPeerSeqDuplicates,
		sSYNetPeerSeqOutOfOrder,
		sSYNetPeerLastPeerAckTick,
		(sSYNetPeerLastPacketTicksValid != FALSE) ? sSYNetPeerLastPacketOldestTick : (~(u32)0),
		(sSYNetPeerLastPacketTicksValid != FALSE) ? sSYNetPeerLastPacketNewestTick : (~(u32)0),
		sSYNetPeerPacketsSent,
		sSYNetPeerPacketsReceived,
		sSYNetPeerPacketsDropped,
		sSYNetPeerFramesStaged,
		sSYNetPeerHighestRemoteTick,
		sSYNetPeerLateFrames,
		sSYNetPeerInputChecksum,
		sSYNetPeerLastPacketTicksValid);
}
#endif

void syNetPeerLogStats(void)
{
#ifdef PORT
	u32 tick = syNetInputGetTick();
	u32 rollbacks, resim_ticks, max_depth, stalls, timesync_skips;

	if ((tick == 0) || ((tick - sSYNetPeerLastLogTick) < SYNETPEER_LOG_INTERVAL))
	{
		return;
	}
	sSYNetPeerLastLogTick = tick;
	syNetStateGetStats(&rollbacks, &resim_ticks, &max_depth, &stalls, &timesync_skips);

	port_log("SSB64 NetPeer: role=%s local=%d remote=%d ping=%ums delay=%u rollbacks=%u resim=%u max_depth=%u stalls=%u skips=%u barrier=%d execution_ready=%d tick=%u sent=%u recv=%u dropped=%u staged=%u highest_remote=%u late=%u snd_next=%u rcv_hw=%u seq_gap=%u seq_dup=%u seq_ooo=%u peer_ack=%u inpchk=0x%08X\n",
	         (sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client",
	         sSYNetPeerLocalPlayer, sSYNetPeerRemotePlayer, syNetPeerGetPingMs(), sSYNetPeerInputDelay,
	         rollbacks, resim_ticks, max_depth, stalls, timesync_skips,
	         sSYNetPeerBattleBarrierReleased, syNetPeerCheckBattleExecutionReady(), tick,
	         sSYNetPeerPacketsSent, sSYNetPeerPacketsReceived, sSYNetPeerPacketsDropped,
	         sSYNetPeerFramesStaged, sSYNetPeerHighestRemoteTick, sSYNetPeerLateFrames,
	         sSYNetPeerSendSeq, sSYNetPeerRecvSeqHighWater, sSYNetPeerSeqGaps,
	         sSYNetPeerSeqDuplicates, sSYNetPeerSeqOutOfOrder,
	         sSYNetPeerLastPeerAckTick,
	         sSYNetPeerInputChecksum);

	syNetPeerLogNetSyncValidation(tick);
#endif
}

void syNetPeerLogExecutionHold(void)
{
#ifdef PORT
	if ((sSYNetPeerExecutionHoldFrames == 1) ||
		((sSYNetPeerExecutionHoldFrames % SYNETPEER_BARRIER_LOG_INTERVAL) == 0))
	{
		port_log("SSB64 NetPeer: execution hold role=%s local=%d remote=%d tick=%u hold=%u barrier_wait=%u peer_ready=%d start_sent=%d start_recv=%d highest_remote=%u late=%u\n",
		         (sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client",
		         sSYNetPeerLocalPlayer, sSYNetPeerRemotePlayer, syNetInputGetTick(),
		         sSYNetPeerExecutionHoldFrames, sSYNetPeerBattleBarrierWaitFrames,
		         sSYNetPeerBattlePeerReady, sSYNetPeerBattleStartSent,
		         sSYNetPeerBattleStartReceived, sSYNetPeerHighestRemoteTick,
		         sSYNetPeerLateFrames);
	}
#endif
}

void syNetPeerLogExecutionBegin(void)
{
#ifdef PORT
	if (sSYNetPeerExecutionBeginLogged == FALSE)
	{
		sSYNetPeerExecutionBeginLogged = TRUE;
		port_log("SSB64 NetPeer: execution begin role=%s local=%d remote=%d tick=%u hold=%u barrier_wait=%u highest_remote=%u late=%u\n",
		         (sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client",
		         sSYNetPeerLocalPlayer, sSYNetPeerRemotePlayer, syNetInputGetTick(),
		         sSYNetPeerExecutionHoldFrames, sSYNetPeerBattleBarrierWaitFrames,
		         sSYNetPeerHighestRemoteTick, sSYNetPeerLateFrames);
	}
#endif
}

void syNetPeerLogBarrierWait(void)
{
#ifdef PORT
	if ((sSYNetPeerBattleBarrierWaitFrames % SYNETPEER_BARRIER_LOG_INTERVAL) == 0)
	{
		port_log("SSB64 NetPeer: barrier wait role=%s local=%d remote=%d tick=%u local_ready=%d peer_ready=%d start_sent=%d start_recv=%d sent=%u recv=%u dropped=%u staged=%u highest_remote=%u late=%u\n",
		         (sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client",
		         sSYNetPeerLocalPlayer, sSYNetPeerRemotePlayer, syNetInputGetTick(),
		         sSYNetPeerBattleLocalReady, sSYNetPeerBattlePeerReady,
		         sSYNetPeerBattleStartSent, sSYNetPeerBattleStartReceived, sSYNetPeerPacketsSent,
		         sSYNetPeerPacketsReceived, sSYNetPeerPacketsDropped,
		         sSYNetPeerFramesStaged, sSYNetPeerHighestRemoteTick, sSYNetPeerLateFrames);
	}
#endif
}

void syNetPeerReleaseBattleBarrier(const char *reason)
{
	if (sSYNetPeerBattleBarrierReleased == FALSE)
	{
		sSYNetPeerBattleBarrierReleased = TRUE;

#ifdef PORT
		port_log("SSB64 NetPeer: barrier release role=%s reason=%s local=%d remote=%d tick=%u wait=%u sent=%u recv=%u dropped=%u staged=%u highest_remote=%u late=%u\n",
		         (sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client", reason,
		         sSYNetPeerLocalPlayer, sSYNetPeerRemotePlayer, syNetInputGetTick(),
		         sSYNetPeerBattleBarrierWaitFrames, sSYNetPeerPacketsSent,
		         sSYNetPeerPacketsReceived, sSYNetPeerPacketsDropped,
		         sSYNetPeerFramesStaged, sSYNetPeerHighestRemoteTick, sSYNetPeerLateFrames);
#endif
	}
}

void syNetPeerUpdateStartBarrier(void)
{
#ifdef PORT
	if ((sSYNetPeerBattleBarrierEnabled == FALSE) || (sSYNetPeerBattleBarrierReleased != FALSE))
	{
		return;
	}
	sSYNetPeerBattleBarrierWaitFrames++;

	/* Watching: nobody waits on us, so nothing to send. Go once every
	 * player's tick 0 is here (sys/netstate.c holds the ticks after it). */
	if (sSYNetPeerLocalPlayer < 0)
	{
		if (syNetInputCheckRemoteConfirmed(0) != FALSE)
		{
			syNetPeerReleaseBattleBarrier("spectator");
		}
		else syNetPeerLogBarrierWait();

		return;
	}
	syNetPeerSendControlPacket(SYNETPEER_PACKET_BATTLE_READY);

	/* The host waits for every player in the round (not the spectators,
	 * whose game just follows the inputs). */
	if (sSYNetPeerBootstrapIsHost != FALSE)
	{
		if (syNetPeerCheckRoundMembers(1, TRUE) != FALSE)
		{
			syNetPeerSendControlPacket(SYNETPEER_PACKET_BATTLE_START);
			sSYNetPeerBattleStartSent = TRUE;

			if (syNetPeerCheckRoundMembers(2, TRUE) != FALSE)
			{
				sSYNetPeerBattleStartRepeatFrames = SYNETPEER_BATTLE_START_REPEAT_FRAMES;
				syNetPeerReleaseBattleBarrier("client-ack");
			}
		}
	}
	else if (sSYNetPeerBattlePeerReady != FALSE)
	{
		if (sSYNetPeerBattleStartReceived != FALSE)
		{
			syNetPeerSendControlPacket(SYNETPEER_PACKET_BATTLE_START);
			sSYNetPeerBattleStartSent = TRUE;
			sSYNetPeerBattleStartRepeatFrames = SYNETPEER_BATTLE_START_REPEAT_FRAMES;
			syNetPeerReleaseBattleBarrier("host-start");
		}
	}
	syNetPeerLogBarrierWait();
#endif
}

void syNetPeerUpdateBattleGate(void)
{
	if (sSYNetPeerIsActive == FALSE)
	{
		return;
	}
#ifdef PORT
	/* Receives too. A round applied here (a player dropped) resets the
	 * battle scene; the rest of this tick no longer matters. */
	if (syNetPeerServiceRound() != FALSE)
	{
		return;
	}
#endif
	syNetPeerReceiveRemoteInput();
	syNetPeerUpdateStartBarrier();

	if (syNetPeerCheckBattleExecutionReady() == FALSE)
	{
		sSYNetPeerExecutionHoldFrames++;
		syNetPeerLogExecutionHold();
	}
	else syNetPeerLogExecutionBegin();
}

void syNetPeerUpdate(void)
{
	if (sSYNetPeerIsActive == FALSE)
	{
		return;
	}
	syNetPeerUpdateBattleGate();

	if (syNetPeerCheckBattleExecutionReady() == FALSE)
	{
		return;
	}
	if (sSYNetPeerBattleStartRepeatFrames != 0)
	{
		syNetPeerSendControlPacket(SYNETPEER_PACKET_BATTLE_START);
		sSYNetPeerBattleStartRepeatFrames--;
	}
	syNetPeerSendLocalInput();
#ifdef PORT
	syNetPeerSendPeriodicPing();
#endif
	syNetPeerLogStats();
}

/* ---- HUD accessors (port/netplay/overlay.cpp) ---- */
sb32 syNetPeerCheckActive(void)
{
	return sSYNetPeerIsActive;
}

/* A guest: to the host. The host: the slowest player in the round. */
u32 syNetPeerGetPingMs(void)
{
#ifdef PORT
	u32 ping = 0;
	s32 i;

	if (sSYNetPeerIsHub == FALSE)
	{
		return sSYNetPeerMembers[0].ping_ms;
	}
	for (i = 1; i < SYNETPEER_MAX_MEMBERS; i++)
	{
		SYNetPeerMember *m = &sSYNetPeerMembers[i];

		if ((m->is_used != FALSE) && (m->is_in_round != FALSE) && (m->slot >= 0) && (m->ping_ms > ping))
		{
			ping = m->ping_ms;
		}
	}
	return ping;
#else
	return sSYNetPeerPingMs;
#endif
}

u32 syNetPeerGetInputDelay(void)
{
	return sSYNetPeerInputDelay;
}

/* Ticks our simulation has run past the newest input we hold from the peer.
 * Positive = we are ahead (predicting), negative = behind. */
s32 syNetPeerGetLocalAdvantage(void)
{
	if ((sSYNetPeerIsActive == FALSE) || (sSYNetPeerLocalPlayer < 0))
	{
		return 0;
	}
	return (s32)syNetInputGetTick() - (s32)sSYNetPeerHighestRemoteTick;
}

/* How much further ahead we are than the peer says it is; 0 until the
 * first packet. Positive means we should give way. */
s32 syNetPeerGetAdvantageDelta(void)
{
	s32 lowest = 0;
	sb32 any = FALSE;
	s32 slot;

	if ((sSYNetPeerIsActive == FALSE) || (sSYNetPeerLocalPlayer < 0))
	{
		return 0;
	}
	/* Against the player furthest behind: we give way to the slowest. */
	for (slot = 0; slot < MAXCONTROLLERS; slot++)
	{
		if ((sSYNetPeerRemoteMask & (1 << slot)) && (sSYNetPeerSlotAdvantageValid[slot] != FALSE))
		{
			if ((any == FALSE) || (sSYNetPeerSlotAdvantage[slot] < lowest))
			{
				lowest = sSYNetPeerSlotAdvantage[slot];
				any = TRUE;
			}
		}
	}
	if (any == FALSE)
	{
		return 0;
	}
	return syNetPeerGetLocalAdvantage() - lowest;
}

/* Called by sys/netstate.c instead of the tick while it stalls for a peer
 * that has fallen further behind than the snapshot ring can roll back to:
 * keep receiving (their inputs are what ends the stall) and keep sending
 * (our queued inputs are what they are waiting for), nothing else. */
void syNetPeerPumpWhileStalled(void)
{
	if (sSYNetPeerIsActive == FALSE)
	{
		return;
	}
#ifdef PORT
	if (syNetPeerServiceRound() != FALSE)
	{
		return;
	}
#endif
	syNetPeerReceiveRemoteInput();
	syNetPeerSendLocalInput();
#ifdef PORT
	/* A spectator sends no input: its pings are how the host knows it is
	 * still there. */
	if (sSYNetPeerLocalPlayer < 0)
	{
		syNetPeerSendPeriodicPing();
	}
#endif
}

void syNetPeerStopVSSession(void)
{
#ifdef PORT
	if (sSYNetPeerIsActive != FALSE)
	{
		port_log("SSB64 NetPeer: VS session stop sent=%u recv=%u dropped=%u staged=%u late=%u checksum=0x%08X\n",
		         sSYNetPeerPacketsSent, sSYNetPeerPacketsReceived, sSYNetPeerPacketsDropped,
		         sSYNetPeerFramesStaged, sSYNetPeerLateFrames, sSYNetPeerInputChecksum);
	}
	if (sSYNetPeerMenuMode != FALSE)
	{
		s32 i;

		/* Not a battle of this session's (a round reset already left it, or
		 * it never started): nothing to hand back. */
		if ((sSYNetPeerIsActive == FALSE) || (sSYNetPeerBattleRunning == FALSE))
		{
			return;
		}
		sSYNetPeerBattleRunning = FALSE;

		/* The peer may still be a few ticks short of the end: repeat the last
		 * input packet, since nothing retransmits once the battle is gone. */
		for (i = 0; i < SYNETPEER_STOP_REPEAT_PACKETS; i++)
		{
			syNetPeerSendLocalInput();
		}
		/* Back to the menus (results, then the character select) in
		 * lockstep; the connection stays up for the next game. */
		syNetPeerBeginPhase();
		syNetMenuStart(sSYNetPeerLocalPlayer, sSYNetPeerRemoteMask, sSYNetPeerInputDelay, -1,
		               sSYNetPeerBootstrapSeed ^ (sSYNetPeerSessionPhase * 2654435761U), FALSE);
		syNetPeerApplyEarlyFrames();
		return;
	}
	if ((sSYNetPeerLobbySession != FALSE) && (sSYNetPeerIsActive != FALSE) && (sSYNetPeerPostMatch == FALSE))
	{
		extern void port_lobby_notify_post_match(void);

		/* A lobby match keeps its hole-punched socket so the players can
		 * go again without the server; see syNetPeerRematchStep. */
		sSYNetPeerPostMatch = TRUE;
		sSYNetPeerRematchLocal = FALSE;
		sSYNetPeerRematchPeer = FALSE;
		sSYNetPeerIsActive = FALSE;
		port_lobby_notify_post_match();
		return;
	}
	if (sSYNetPeerLobbySession == FALSE)
	{
		syNetPeerCloseSocket();
	}
#endif
	sSYNetPeerIsActive = FALSE;
}
