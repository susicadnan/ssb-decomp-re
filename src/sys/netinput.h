#ifndef _SYNETINPUT_H_
#define _SYNETINPUT_H_

#include <PR/ultratypes.h>
#include <sys/controller.h>

#define SYNETINPUT_HISTORY_LENGTH 720
#define SYNETINPUT_REPLAY_MAX_FRAMES 21600
#define SYNETINPUT_REPLAY_MAGIC 0x53534E52 // SSNR
#define SYNETINPUT_REPLAY_VERSION 1

typedef enum SYNetInputSource
{
	nSYNetInputSourceLocal,
	nSYNetInputSourceRemoteConfirmed,
	nSYNetInputSourceRemotePredicted,
	nSYNetInputSourceSaved

} SYNetInputSource;

typedef struct SYNetInputFrame
{
	u32 tick;
	u16 buttons;
	s8 stick_x;
	s8 stick_y;
	u8 source;
	ub8 is_predicted;
	ub8 is_valid;

} SYNetInputFrame;

typedef struct SYNetInputReplayMetadata
{
	u32 magic;
	u32 version;
	u32 scene_kind;
	u32 player_count;
	u32 stage_kind;
	u32 stocks;
	u32 time_limit;
	u32 item_switch;
	u32 item_toggles;
	u32 rng_seed;
	u8 game_type;
	u8 game_rules;
	u8 is_team_battle;
	u8 handicap;
	u8 is_team_attack;
	u8 is_stage_select;
	u8 damage_ratio;
	u8 item_appearance_rate;
	u8 is_not_teamshadows;
	u8 player_kinds[MAXCONTROLLERS];
	u8 fighter_kinds[MAXCONTROLLERS];
	u8 costumes[MAXCONTROLLERS];
	u8 teams[MAXCONTROLLERS];
	u8 handicaps[MAXCONTROLLERS];
	u8 levels[MAXCONTROLLERS];
	u8 shades[MAXCONTROLLERS];

} SYNetInputReplayMetadata;

/* The per-tick bookkeeping rollback rewinds with the simulation (see
 * sys/netstate.c). The history rings themselves are not part of it: they are
 * indexed by tick and simply get overwritten when a tick is re-simulated. */
typedef struct SYNetInputState
{
	u32 tick;
	u32 published_checksum;
	u32 published_tick_count;
	u32 published_checksum_at_limit;
	SYNetInputFrame last_confirmed[MAXCONTROLLERS];
	SYNetInputFrame last_published[MAXCONTROLLERS];

} SYNetInputState;

extern void syNetInputReset(void);
extern void syNetInputStartVSSession(void);
extern u32 syNetInputGetTick(void);
extern void syNetInputSetTick(u32 tick);
extern void syNetInputSetSlotSource(s32 player, SYNetInputSource source);
extern SYNetInputSource syNetInputGetSlotSource(s32 player);
extern void syNetInputSetRemoteInput(s32 player, u32 tick, u16 buttons, s8 stick_x, s8 stick_y);
extern void syNetInputSetSavedInput(s32 player, u32 tick, u16 buttons, s8 stick_x, s8 stick_y);
extern sb32 syNetInputGetHistoryFrame(s32 player, u32 tick, SYNetInputFrame *out_frame);
extern sb32 syNetInputGetPublishedFrame(s32 player, SYNetInputFrame *out_frame);
extern u32 syNetInputGetHistoryChecksum(s32 player, u32 tick_begin, u32 frame_count);
extern u32 syNetInputGetHistoryInputValueChecksumForPlayer(s32 player, u32 tick_begin, u32 frame_count);
extern void syNetInputGetHistoryInputValueChecksumWindow(u32 tick_begin, u32 frame_count, u32 *out_checksums,
                                                       u32 *out_combined_checksum);
extern void syNetInputSetRecordingEnabled(sb32 is_enabled);
extern sb32 syNetInputGetRecordingEnabled(void);
extern u32 syNetInputGetRecordedFrameCount(void);
extern void syNetInputClearReplayFrames(void);
extern sb32 syNetInputSetReplayFrame(s32 player, u32 tick, const SYNetInputFrame *frame);
extern sb32 syNetInputGetReplayFrame(s32 player, u32 tick, SYNetInputFrame *out_frame);
extern u32 syNetInputGetReplayInputChecksum(void);
extern void syNetInputSetPublishedChecksumLimit(u32 tick_limit);
extern u32 syNetInputGetPublishedTickCount(void);
extern u32 syNetInputGetPublishedInputChecksum(void);
extern void syNetInputSetReplayMetadata(const SYNetInputReplayMetadata *metadata);
extern sb32 syNetInputGetReplayMetadata(SYNetInputReplayMetadata *out_metadata);
extern void syNetInputFuncRead(void);
extern void syNetInputSetLocalDelay(u32 delay);
extern u32 syNetInputGetLocalDelay(void);
extern sb32 syNetInputGetLocalQueuedFrame(s32 player, u32 tick, SYNetInputFrame *out_frame);
extern u32 syNetInputGetLocalQueuedCount(s32 player);
extern u32 syNetInputGetRemoteConfirmedCount(s32 player);
extern u32 syNetInputGetRemoteLag(void);
extern sb32 syNetInputCheckRemoteConfirmed(u32 tick);
extern void syNetInputSetLocalHardwarePort(s32 port);
extern sb32 syNetInputGetRemoteFrame(s32 player, u32 tick, SYNetInputFrame *out_frame);
extern void syNetInputQueueLocalFrame(s32 player, u32 tick);
extern void syNetInputPublishFrame(s32 player, SYNetInputFrame *frame);
extern void syNetInputPublishMainController(void);
extern u16 syNetInputGetLocalFlags(s32 port);
extern sb32 syNetInputCheckTapJumpOff(s32 player);
extern void syNetInputMakeFrame(SYNetInputFrame *frame, u32 tick, u16 buttons, s8 stick_x, s8 stick_y,
                                SYNetInputSource source, sb32 is_predicted);
extern void syNetInputSetResimulating(sb32 is_resimulating);
extern sb32 syNetInputGetResimulating(void);
extern void syNetInputSaveState(SYNetInputState *out_state);
extern void syNetInputRestoreState(const SYNetInputState *state);

#endif /* _SYNETINPUT_H_ */
