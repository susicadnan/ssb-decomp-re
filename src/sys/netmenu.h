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
 * Each player of the round has their port (host = player 1 in a 1v1 of
 * two); the other ports read as empty, and anyone watching sends nothing.
 * sys/taskman.c calls syNetMenuFuncRead instead of the scene's controller
 * function while a session is active. */

/* arm_scene: the lockstep ticks begin with the first controller read inside
 * that scene (the one both players are being sent to); -1 = right away. The
 * game's RNG is re-seeded with `seed` on that first tick, so everyone draws
 * the same numbers from then on whatever ran before.
 * local_player -1 = watching; remote_mask = the ports other members play.
 * arm_after_scene_start: a reset into arm_scene is pending, so wait for the
 * scene to start again (syNetMenuNoteSceneStart) even if it is the current
 * one. */
extern void syNetMenuStart(s32 local_player, u32 remote_mask, u32 delay, s32 arm_scene, u32 seed, sb32 arm_after_scene_start);
extern void syNetMenuNoteSceneStart(void);
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
