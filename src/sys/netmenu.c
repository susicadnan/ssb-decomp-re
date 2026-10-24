#include <sys/netmenu.h>

#include <ft/fighter.h>
#include <gr/ground.h>
#include <sc/scmanager.h>
#include <sc/sctypes.h>
#include <sys/controller.h>
#include <sys/netinput.h>
#include <sys/netpeer.h>

#ifdef PORT
extern void port_log(const char *fmt, ...);
extern char *getenv(const char *name);
extern int atoi(const char *s);
extern sb32 sSYNetInputFakeLocal; /* sys/netinput.c */
#endif

/* Frames of consecutive waiting before the peer counts as gone (10 s). */
#define SYNETMENU_DISCONNECT_FRAMES 600
#define SYNETMENU_LOG_INTERVAL 600

sb32 sSYNetMenuIsActive;
s32 sSYNetMenuArmScene;  /* -1 once ticking */
s32 sSYNetMenuLocalPlayer;
s32 sSYNetMenuRemotePlayer;
u32 sSYNetMenuDelay;
u32 sSYNetMenuWaitRun;   /* consecutive frames spent waiting for the peer */
u32 sSYNetMenuWaitTotal;
u32 sSYNetMenuTicks;
/* SSB64_NETMENU_BOT=1 (PORT debug): the local player presses a fixed
 * pattern that walks the VS menus into a battle without a human: push the
 * stick up, press A, press START, repeat. */
sb32 sSYNetMenuBot;

static s32 syNetMenuHardwarePort(void)
{
	return (gSYControllerDeviceStatuses[0] >= 0) ? gSYControllerDeviceStatuses[0] : 0;
}

void syNetMenuStart(s32 local_player, s32 remote_player, u32 delay, s32 arm_scene)
{
	syNetInputReset();
	syNetInputSetSlotSource(local_player, nSYNetInputSourceLocal);
	syNetInputSetSlotSource(remote_player, nSYNetInputSourceRemoteConfirmed);
	syNetInputSetLocalDelay(delay);
	/* Whoever we are, our controller is in port 1 (the first connected one). */
	syNetInputSetLocalHardwarePort(syNetMenuHardwarePort());
	syNetInputSetResimulating(FALSE);

	sSYNetMenuLocalPlayer = local_player;
	sSYNetMenuRemotePlayer = remote_player;
	sSYNetMenuDelay = delay;
	sSYNetMenuArmScene = arm_scene;
	sSYNetMenuWaitRun = 0;
	sSYNetMenuWaitTotal = 0;
	sSYNetMenuTicks = 0;
	sSYNetMenuIsActive = TRUE;
#ifdef PORT
	{
		const char *env = getenv("SSB64_NETMENU_BOT");

		sSYNetMenuBot = ((env != NULL) && (atoi(env) != 0)) ? TRUE : FALSE;

		/* The bot drives the menus through the hardware slots; the battle's
		 * synthetic inputs (SSB64_NETINPUT_FAKE) come back with the battle's
		 * own netinput reset. */
		if (sSYNetMenuBot != FALSE)
		{
			sSYNetInputFakeLocal = FALSE;
		}
	}
	port_log("SSB64 NetMenu: lockstep start local=%d remote=%d delay=%u arm_scene=%d\n", local_player,
	         remote_player, delay, arm_scene);
#endif
}

void syNetMenuStop(void)
{
#ifdef PORT
	if (sSYNetMenuIsActive != FALSE)
	{
		port_log("SSB64 NetMenu: lockstep stop after %u ticks, waited %u frames\n", sSYNetMenuTicks,
		         sSYNetMenuWaitTotal);
	}
#endif
	sSYNetMenuIsActive = FALSE;
	syNetInputSetLocalHardwarePort(-1);
}

sb32 syNetMenuCheckActive(void)
{
	return sSYNetMenuIsActive;
}

u32 syNetMenuGetTick(void)
{
	return syNetInputGetTick();
}

u32 syNetMenuGetWaitFrames(void)
{
	return sSYNetMenuWaitRun;
}

/* For the port's in-game Online panel (port/netplay/ingame_ui.cpp), which
 * has no view of the scene enum: the menus where it makes sense to offer a
 * connection. */
sb32 syNetMenuCheckOnlinePanelScene(void)
{
	u8 scene = gSCManagerSceneData.scene_curr;

	return ((scene == nSCKindModeSelect) || (scene == nSCKindVSMode)) ? TRUE : FALSE;
}

sb32 syNetMenuCheckBattleScene(void)
{
	return (gSCManagerSceneData.scene_curr == nSCKindVSBattle) ? TRUE : FALSE;
}

#ifdef PORT
static sb32 syNetMenuBotIn(u32 phase, u32 from, u32 to)
{
	return ((phase >= from) && (phase < to)) ? TRUE : FALSE;
}

/* A scene-aware walk into a battle. VS menu: A on the highlighted VS Start.
 * Character select: stick down to come back onto the token, A to grab it,
 * carry it up onto the roster, A to drop, START. Stage select: A at a few
 * places. Results: START. Options: B back out. */
static void syNetMenuBotOverride(u32 tick)
{
	SYController *c = &gSYControllerDevices[syNetMenuHardwarePort()];
	u32 phase;

	c->button_hold = 0;
	c->stick_range.x = 0;
	c->stick_range.y = 0;

	switch (gSCManagerSceneData.scene_curr)
	{
	case nSCKindPlayersVS:
	{
		/* mn/mnplayers/mnplayersvs.c knows where the cursor and token are. */
		extern void portMNPlayersVSBotDrive(s32 player, u16 *buttons, s8 *stick_x, s8 *stick_y);

		portMNPlayersVSBotDrive(sSYNetMenuLocalPlayer, &c->button_hold, &c->stick_range.x, &c->stick_range.y);
		break;
	}

	case nSCKindMaps:
		phase = tick % 200;
		if (syNetMenuBotIn(phase, 0, 20)) c->stick_range.y = 70;
		else if (syNetMenuBotIn(phase, 30, 32)) c->button_hold = A_BUTTON;
		else if (syNetMenuBotIn(phase, 40, 60)) c->stick_range.x = 70;
		else if (syNetMenuBotIn(phase, 70, 72)) c->button_hold = A_BUTTON;
		else if (syNetMenuBotIn(phase, 80, 100)) c->stick_range.y = -70;
		else if (syNetMenuBotIn(phase, 110, 112)) c->button_hold = A_BUTTON;
		else if (syNetMenuBotIn(phase, 120, 140)) c->stick_range.x = -70;
		else if (syNetMenuBotIn(phase, 150, 152)) c->button_hold = A_BUTTON;
		else if (syNetMenuBotIn(phase, 170, 172)) c->button_hold = START_BUTTON;
		break;

	case nSCKindVSOptions:
		if ((tick % 60) < 2) c->button_hold = B_BUTTON;
		break;

	case nSCKindVSResults:
		if ((tick % 60) < 2) c->button_hold = START_BUTTON;
		break;

	default:
		/* VS menu (VS Start is highlighted on entry) and anything else. */
		if ((tick % 60) < 2) c->button_hold = A_BUTTON;
		break;
	}
}
#endif

sb32 syNetMenuFuncRead(void)
{
	SYNetInputFrame frame;
	u32 tick = syNetInputGetTick();
	s32 player;

	if (sSYNetMenuIsActive == FALSE)
	{
		return TRUE;
	}
	/* Poll the hardware every frame so the controller thread keeps its
	 * cadence; the values are only sampled once per tick, below. */
	syControllerFuncRead();

	/* Until both players are in the scene they were sent to, the scene
	 * being torn down keeps its own controllers and no tick is spent. */
	if (sSYNetMenuArmScene >= 0)
	{
		if (gSCManagerSceneData.scene_curr != sSYNetMenuArmScene)
		{
			return TRUE;
		}
		sSYNetMenuArmScene = -1;
#ifdef PORT
		port_log("SSB64 NetMenu: ticking from scene %d\n", (int)gSCManagerSceneData.scene_curr);
#endif
	}
#ifdef PORT
	if (sSYNetMenuBot != FALSE)
	{
		syNetMenuBotOverride(tick);
	}
#endif
	if (syNetInputGetLocalQueuedCount(sSYNetMenuLocalPlayer) <= tick + sSYNetMenuDelay)
	{
		syNetInputQueueLocalFrame(sSYNetMenuLocalPlayer, tick);
	}
	/* Receive the peer's frames and (re)send ours. */
	syNetPeerPumpWhileStalled();
#ifdef PORT
	syNetPeerSendPeriodicPing();
#endif

	if (syNetInputGetRemoteFrame(sSYNetMenuRemotePlayer, tick, &frame) == FALSE)
	{
		sSYNetMenuWaitRun++;
		sSYNetMenuWaitTotal++;

		if (sSYNetMenuWaitRun >= SYNETMENU_DISCONNECT_FRAMES)
		{
#ifdef PORT
			port_log("SSB64 NetMenu: no input from the peer for %u frames at tick %u\n", sSYNetMenuWaitRun, tick);
			sSYNetMenuWaitRun = 0;
			syNetPeerAbortMatch("connection lost");
#endif
		}
		return FALSE;
	}
	sSYNetMenuWaitRun = 0;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		if (player == sSYNetMenuRemotePlayer)
		{
			syNetInputPublishFrame(player, &frame);
		}
		else if ((player == sSYNetMenuLocalPlayer)
		         && (syNetInputGetLocalQueuedFrame(player, tick, &frame) != FALSE))
		{
			syNetInputPublishFrame(player, &frame);
		}
		else
		{
			syNetInputMakeFrame(&frame, tick, 0, 0, 0, nSYNetInputSourceLocal, FALSE);
			syNetInputPublishFrame(player, &frame);
		}
	}
	/* Exactly two controllers, in ports 1 and 2, on both machines. */
	gSYControllerDeviceStatuses[0] = 0;
	gSYControllerDeviceStatuses[1] = 1;
	gSYControllerDeviceStatuses[2] = -1;
	gSYControllerDeviceStatuses[3] = -1;
	gSYControllerConnectedNum = 2;
	syNetInputPublishMainController();

	syNetInputSetTick(tick + 1);
	sSYNetMenuTicks++;
#ifdef PORT
	if ((sSYNetMenuTicks % SYNETMENU_LOG_INTERVAL) == 0)
	{
		port_log("SSB64 NetMenu: tick %u scene %d, waited %u frames so far\n", tick + 1,
		         (int)gSCManagerSceneData.scene_curr, sSYNetMenuWaitTotal);
	}
#endif
	return TRUE;
}
