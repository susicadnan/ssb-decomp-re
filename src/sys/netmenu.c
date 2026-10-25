#include <sys/netmenu.h>

#include <ft/fighter.h>
#include <gr/ground.h>
#include <sc/scmanager.h>
#include <sc/sctypes.h>
#include <sys/controller.h>
#include <sys/netinput.h>
#include <sys/netpeer.h>
#include <sys/utils.h>

#ifdef PORT
extern void port_log(const char *fmt, ...);
extern char *getenv(const char *name);
extern int atoi(const char *s);
extern sb32 sSYNetInputFakeLocal; /* sys/netinput.c */
extern void port_coroutine_yield(void);
extern u32 port_netsock_now_ms(void);
#endif

/* Frames of consecutive waiting before the peer counts as gone (10 s). */
#define SYNETMENU_DISCONNECT_FRAMES 600
/* The port measures it on the clock instead: a yield that draws no new
 * image comes back far sooner than a 60 Hz frame (hundreds a second
 * without vsync), so 600 of them was about a second. */
#define SYNETMENU_DISCONNECT_MS 10000
#define SYNETMENU_LOG_INTERVAL 600

sb32 sSYNetMenuIsActive;
s32 sSYNetMenuArmScene;  /* -1 once ticking */
u32 sSYNetMenuSeed;
sb32 sSYNetMenuArmNeedsStart; /* the arm scene must be started anew first (a reset is under way) */
s32 sSYNetMenuLocalPlayer;   /* -1 = watching */
u32 sSYNetMenuRemoteMask;    /* ports someone else plays */
s32 sSYNetMenuLastScene;
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

void syNetMenuStart(s32 local_player, u32 remote_mask, u32 delay, s32 arm_scene, u32 seed, sb32 arm_after_scene_start)
{
	s32 player;

	sSYNetMenuSeed = seed;
	syNetInputReset();

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		if (remote_mask & (1 << player))
		{
			syNetInputSetSlotSource(player, nSYNetInputSourceRemoteConfirmed);
		}
	}
	if (local_player >= 0)
	{
		syNetInputSetSlotSource(local_player, nSYNetInputSourceLocal);
	}
	syNetInputSetLocalDelay(delay);
	/* Whoever we are, our controller is in port 1 (the first connected one). */
	syNetInputSetLocalHardwarePort(syNetMenuHardwarePort());
	syNetInputSetResimulating(FALSE);

	sSYNetMenuLocalPlayer = local_player;
	sSYNetMenuRemoteMask = remote_mask;
	sSYNetMenuDelay = delay;
	sSYNetMenuArmScene = arm_scene;
	sSYNetMenuArmNeedsStart = arm_after_scene_start;
	sSYNetMenuLastScene = -1;
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
	port_log("SSB64 NetMenu: lockstep start local=%d remote_mask=0x%x delay=%u arm_scene=%d after_start=%d\n",
	         local_player, remote_mask, delay, arm_scene, arm_after_scene_start);
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

/* sys/taskman.c, at every scene start. */
void syNetMenuNoteSceneStart(void)
{
	sSYNetMenuArmNeedsStart = FALSE;
}

/* Ports that hold a player this round: the menus see a controller there. */
static u32 syNetMenuPlayerMask(void)
{
	return sSYNetMenuRemoteMask | ((sSYNetMenuLocalPlayer >= 0) ? (1 << sSYNetMenuLocalPlayer) : 0);
}

static void syNetMenuPublishDeviceStatuses(void)
{
	u32 mask = syNetMenuPlayerMask();
	s32 player;

	gSYControllerConnectedNum = 0;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		if (mask & (1 << player))
		{
			gSYControllerDeviceStatuses[player] = player;
			gSYControllerConnectedNum++;
		}
		else gSYControllerDeviceStatuses[player] = -1;
	}
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
#ifdef PORT
	u32 wait_start_ms = 0;
#endif

	if (sSYNetMenuIsActive == FALSE)
	{
		return TRUE;
	}
	/* Poll the hardware every frame so the controller thread keeps its
	 * cadence; the values are only sampled once per tick, below. */
	syControllerFuncRead();

	/* Until everyone is in the scene they were sent to, the scene being torn
	 * down keeps its own controllers and no tick is spent. After a reset the
	 * scene must also have started again: the old instance of the same
	 * scene may still run a frame. */
	if (sSYNetMenuArmScene >= 0)
	{
		if ((sSYNetMenuArmNeedsStart != FALSE) || (gSCManagerSceneData.scene_curr != sSYNetMenuArmScene))
		{
			return TRUE;
		}
		sSYNetMenuArmScene = -1;
#ifdef PORT
		port_log("SSB64 NetMenu: ticking from scene %d\n", (int)gSCManagerSceneData.scene_curr);
#endif
	}
	/* A round exchange (sys/netpeer.c) holds the menus until it is through;
	 * applying a round restarts this lockstep for the scene it resets to. */
	if ((syNetPeerServiceRound() != FALSE) || (syNetPeerCheckRoundBusy() != FALSE))
	{
		return FALSE;
	}
	/* The results screen just gave way to the character select: a round
	 * ends here (every member sees it on the same tick). */
	if ((sSYNetMenuLastScene == nSCKindVSResults) && (gSCManagerSceneData.scene_curr != nSCKindVSResults))
	{
		sSYNetMenuLastScene = gSCManagerSceneData.scene_curr;
		syNetPeerOnMatchCycleEnd();

		if (syNetPeerCheckRoundBusy() != FALSE)
		{
			return FALSE;
		}
	}
	sSYNetMenuLastScene = gSCManagerSceneData.scene_curr;
	/* First shared tick: the two machines ran different things until here
	 * (each was somewhere else when the connection came up), so align the
	 * RNG now; the character select spreads the tokens with it. */
	if (sSYNetMenuTicks == 0)
	{
		syUtilsSetRandomSeed((s32)sSYNetMenuSeed);
	}
#ifdef PORT
	if ((sSYNetMenuBot != FALSE) && (sSYNetMenuLocalPlayer >= 0))
	{
		syNetMenuBotOverride(tick);
	}
#endif
	if ((sSYNetMenuLocalPlayer >= 0) && (syNetInputGetLocalQueuedCount(sSYNetMenuLocalPlayer) <= tick + sSYNetMenuDelay))
	{
		syNetInputQueueLocalFrame(sSYNetMenuLocalPlayer, tick);
	}
	/* Wait for the peer's frame for this tick without leaving the tick: a
	 * yield hands the frame back to the port (it shows the previous image
	 * again) and resumes here. Skipping the update and letting the scene
	 * draw instead would run the menus' display callbacks an extra time on
	 * the side that waits longer, and the character select moves its tokens
	 * from one of those; the two machines then disagree. */
	sSYNetMenuWaitRun = 0;

	for (;;)
	{
		/* Receive everyone's frames and (re)send ours. */
		syNetPeerPumpWhileStalled();
#ifdef PORT
		syNetPeerSendPeriodicPing();
#endif
		if (sSYNetMenuIsActive == FALSE)
		{
			return TRUE; /* the session ended while we waited */
		}
		/* A round came through while we waited: everyone is being reset
		 * into a scene and this lockstep starts over there. */
		if (sSYNetMenuArmScene >= 0)
		{
			return FALSE;
		}
		for (player = 0; player < MAXCONTROLLERS; player++)
		{
			if ((sSYNetMenuRemoteMask & (1 << player)) && (syNetInputGetRemoteFrame(player, tick, &frame) == FALSE))
			{
				break;
			}
		}
		if (player == MAXCONTROLLERS)
		{
			break;
		}
#ifdef PORT
		if (sSYNetMenuWaitRun == 0)
		{
			wait_start_ms = port_netsock_now_ms();
		}
#endif
		sSYNetMenuWaitRun++;
		sSYNetMenuWaitTotal++;

#ifdef PORT
		if ((port_netsock_now_ms() - wait_start_ms) >= SYNETMENU_DISCONNECT_MS)
#else
		if (sSYNetMenuWaitRun >= SYNETMENU_DISCONNECT_FRAMES)
#endif
		{
#ifdef PORT
			port_log("SSB64 NetMenu: no input from port %d for %u ms (%u waits) at tick %u\n", player + 1,
			         port_netsock_now_ms() - wait_start_ms, sSYNetMenuWaitRun, tick);
			sSYNetMenuWaitRun = 0;
			syNetPeerReportStall("connection lost");
#endif
			return FALSE;
		}
#ifdef PORT
		port_coroutine_yield();
#else
		return FALSE;
#endif
	}

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		if ((sSYNetMenuRemoteMask & (1 << player)) && (syNetInputGetRemoteFrame(player, tick, &frame) != FALSE))
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
	/* The same controllers in the same ports on every machine: one per
	 * player of this round, none for anyone watching. */
	syNetMenuPublishDeviceStatuses();
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
