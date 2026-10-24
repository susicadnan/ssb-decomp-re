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
                                    u32 seed, u8 host_fighter, u8 client_fighter, u32 stage_kind);
extern s32 syNetPeerLobbyStep(void);
extern void syNetPeerLobbyAbort(void);
extern void syNetPeerAbortMatch(const char *reason);
extern void syNetPeerRematchRequest(u8 my_fighter);
extern s32 syNetPeerRematchStep(void);
extern sb32 syNetPeerCheckPostMatch(void);
extern sb32 syNetPeerCheckPeerWantsRematch(void);
#endif

#endif /* _SYNETPEER_H_ */
