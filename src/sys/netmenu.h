#ifndef _SYNETMENU_H_
#define _SYNETMENU_H_

#include <PR/ultratypes.h>
#include <ssb_types.h>

/* Netplay in the game's own menus. After the lobby has connected two
 * players, both machines run the VS menus (mode, rules, character select,
 * stage select) in lockstep: each tick is simulated only once the other
 * player's input for it has arrived, so both see the same cursors, picks
 * and rules without any menu code knowing about the network. The battle
 * itself then runs under rollback (sys/netstate.c) as before, and the
 * results screen and the next character select come back here.
 *
 * Host = player 1, client = player 2; the other two ports read as empty.
 * sys/taskman.c calls syNetMenuFuncRead instead of the scene's controller
 * function while a session is active. */

/* arm_scene: the lockstep ticks begin with the first controller read inside
 * that scene (the one both players are being sent to); -1 = right away. The
 * game's RNG is re-seeded with `seed` on that first tick, so both sides draw
 * the same numbers from then on whatever ran before. */
extern void syNetMenuStart(s32 local_player, s32 remote_player, u32 delay, s32 arm_scene, u32 seed);
extern void syNetMenuStop(void);
extern sb32 syNetMenuCheckActive(void);
/* TRUE: both players' inputs for this tick are in gSYControllerDevices, run
 * the scene update. FALSE: still waiting for the peer, draw the same frame
 * again. */
extern sb32 syNetMenuFuncRead(void);
extern u32 syNetMenuGetTick(void);
extern u32 syNetMenuGetWaitFrames(void);
extern sb32 syNetMenuCheckOnlinePanelScene(void);
extern sb32 syNetMenuCheckBattleScene(void);

#endif /* _SYNETMENU_H_ */
