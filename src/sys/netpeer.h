#ifndef _SYNETPEER_H_
#define _SYNETPEER_H_

#include <PR/ultratypes.h>
#include <ssb_types.h>

extern void syNetPeerInitDebugEnv(void);
extern void syNetPeerStartVSSession(void);
extern sb32 syNetPeerCheckBattleExecutionReady(void);
extern sb32 syNetPeerCheckStartBarrierReleased(void);
extern void syNetPeerUpdateBattleGate(void);
extern void syNetPeerUpdate(void);
extern void syNetPeerPumpWhileStalled(void);
extern s32 syNetPeerGetLocalAdvantage(void);
extern s32 syNetPeerGetAdvantageDelta(void);
extern sb32 syNetPeerCheckActive(void);
extern u32 syNetPeerGetPingMs(void);
extern u32 syNetPeerGetInputDelay(void);
extern void syNetPeerStopVSSession(void);

#ifdef PORT
#include <netsock.h>
/* Lobby-driven session: see the comment block in netpeer.c. */
extern void syNetPeerLobbyConfigure(intptr_t sock, const PortNetAddr *peer, sb32 is_host, u32 delay, u32 session_id,
                                    u32 seed, u8 host_fighter, u8 client_fighter, u32 stage_kind, u32 stocks,
                                    u32 time_minutes, sb32 menu_mode);
extern s32 syNetPeerLobbyStep(void);
extern sb32 syNetPeerCheckMenuMode(void);
extern void syNetPeerSendPeriodicPing(void);
extern void syNetPeerLobbyAbort(void);
extern void syNetPeerAbortMatch(const char *reason);
extern void syNetPeerRematchRequest(u8 my_fighter);
extern s32 syNetPeerRematchStep(void);
extern sb32 syNetPeerCheckPostMatch(void);
extern sb32 syNetPeerCheckPeerWantsRematch(void);

/* Group sessions: up to four people, the host is the hub (see netpeer.c). */
#define SYNETPEER_MODE_ROTATION 0 /* 1v1, winner stays on, the rest watch */
#define SYNETPEER_MODE_EVERYONE 1 /* everyone connected plays */
extern void syNetPeerHostConfigure(intptr_t sock, u32 delay, u32 session_id, u32 seed);
extern s32 syNetPeerHostAddMember(const PortNetAddr *addr, const char *code);
extern void syNetPeerHostStartSession(void);
extern void syNetPeerGuestConfigure(intptr_t sock, const PortNetAddr *host, u32 delay, u32 session_id);
extern sb32 syNetPeerServiceRound(void);
extern void syNetPeerOnMatchCycleEnd(void);
extern sb32 syNetPeerCheckRoundBusy(void);
extern void syNetPeerReportStall(const char *reason);
extern sb32 syNetPeerCheckSpectating(void);
extern sb32 syNetPeerCheckGroupHost(void);
extern s32 syNetPeerGetMemberInfo(s32 member, const char **code, s32 *slot, s32 *queue_pos, sb32 *is_me);
extern const char* syNetPeerGetSlotName(s32 slot);
#endif

#endif /* _SYNETPEER_H_ */
