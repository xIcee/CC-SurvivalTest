/*
    plugin.c
    Survival Test-inspired client rules layer.

    - manages survival/creative gamemodes
    - health, score, and inventory persistence
    - world-specific policy enforcement via MOTD
    - hooks into 2D drawing and game events
*/
#include "PluginAPI.h"
#include "ccst_api.h"
#include "Commands.h"
#include "Constants.h"
#include "Entity.h"
#include "Event.h"
#include "Game.h"
#include "Server.h"
#include "String_.h"
#include "health.h"
#include "motd.h"
#include "icons.h"
#include "policy.h"
#include "blockbreak.h"
#include "blockvalue.h"
#include "fakeinventory.h"
#include "score.h"
#include "deathscreen.h"
#include "texturenoise.h"
#include "tilefood.h"
#include "netshim.h"
#include "peer.h"
#include "persist.h"

static void CCST_OnConnected(void* obj);
static void CCST_OnMapLoaded(void* obj);
static void CCST_OnHacksChanged(void* obj);
static void CCST_OnContextLost(void* obj);

// identification suffix for servers
static void CCST_AppendSoftwareNameSuffix(void) {
	static cc_string suffix = String_FromConst(" +survival");
	if (String_CaselessContains(&Server.AppName, &suffix)) return;
	String_AppendConst(&Server.AppName, " +survival");
}

// /gamemode survival|creative
static void CCST_CmdGamemode(const cc_string* args, int argsCount) {
	if (!CCST_Policy_AllowGamemodeCommand()) {
		CCST_Chat("&cThis level forces a gamemode.");
		return;
	}
	if (argsCount < 1) {
		CCST_Chat("&cUsage: /gamemode survival|creative");
		return;
	}
	if (String_CaselessEqualsConst(&args[0], "survival")) {
		CCST_Health_SetModeSurvival(true);
		CCST_Chat("&aGamemode: survival");
	} else if (String_CaselessEqualsConst(&args[0], "creative")) {
		CCST_Health_SetModeSurvival(false);
		CCST_Chat("&aGamemode: creative");
	} else {
		CCST_Chat("&cUse survival or creative");
	}
}

// /inv
static void CCST_CmdInv(const cc_string* args, int argsCount) {
	(void)args;
	(void)argsCount;
	if (!CCST_Policy_SurvivalInventoryEnabled()) {
		CCST_Chat("&cThe survival inventory is disabled on this level.");
		return;
	}
	CCST_Inv_SetOpen(!CCST_Inv_IsOpen());
}

// /ccst status|force|reset
static void CCST_CmdCcst(const cc_string* args, int argsCount) {
	if (!argsCount) {
		CCST_Chat("&eUsage: /ccst status|force|reset");
		return;
	}
	if (String_CaselessEqualsConst(&args[0], "status")) {
		cc_bool gs, god, eff;
		int h, sc;
		CCST_MotdForcedGamemode mg;
		gs = CCST_Health_GetModeSurvival();
		h  = CCST_Health_Get();
		sc = CCST_Score_Get();
		mg = CCST_Motd_GetForcedGamemode();
		god = CCST_Motd_IsGodMode();
		eff = CCST_Health_IsSurvivalActive();
		CCST_Chat("&e=== Survival layer ===");
		Chat_Add1("&eUser gamemode survival: &f%b", &gs);
		Chat_Add1("&eEffective survival rules: &f%b", &eff);
		if (mg == CCST_MOTD_GM_CREATIVE)
			CCST_Chat("&eMOTD gamemode: &fcreative &7(forced)");
		else if (mg == CCST_MOTD_GM_SURVIVAL)
			CCST_Chat("&eMOTD gamemode: &fsurvival &7(forced)");
		else
			CCST_Chat("&eMOTD gamemode: &7(none; user setting)");
		Chat_Add1("&eMOTD +god: &f%b", &god);
		Chat_Add1("&eHealth (half-hearts): &f%i", &h);
		Chat_Add1("&eScore: &f%i", &sc);
		CCST_Chat("&eClient software name sent to server includes &f+survival");
		Chat_Add1("&ePending force candidate: &f%b", &(cc_bool){CCST_Persist_HasSuggestedCandidate()});
		return;
	}
	if (String_CaselessEqualsConst(&args[0], "force")) {
		if (CCST_Persist_ForceSuggestedRestore()) {
			CCST_Chat("&ePersist: forced restore of the most likely candidate.");
			CCST_Chat("&eWrong? Use &f/client ccst reset &eto reset save data for this world.");
		} else {
			CCST_Chat("&ePersist: no force candidate is currently available.");
		}
		return;
	}
	if (String_CaselessEqualsConst(&args[0], "reset")) {
		if (CCST_Persist_ResetCurrentWorldSave()) {
			CCST_Chat("&ePersist: reset this world's save data to your current state.");
		} else {
			CCST_Chat("&cPersist: could not reset save data right now.");
		}
		return;
	}
	CCST_Chat("&cUnknown subcommand. Try /ccst status.");
}

static struct ChatCommand gamemode_cmd = {
	"gamemode",
	CCST_CmdGamemode,
	0,
	{ "&a/gamemode survival|creative", "&eSet your client gamemode.", NULL },
	NULL
};

static struct ChatCommand inv_cmd = {
	"inv",
	CCST_CmdInv,
	0,
	{ "&a/inv", "&eToggle the survival inventory.", NULL },
	NULL
};

static struct ChatCommand ccst_cmd = {
	"ccst",
	CCST_CmdCcst,
	0,
	{ "&a/ccst status|force|reset", "&eShow status or manage persistence restore.", NULL },
	NULL
};

// event handlers
static void CCST_OnConnected(void* obj) {
	(void)obj;
	CCST_Policy_Refresh();
	if (!Server.IsSinglePlayer)
		CCST_AppendSoftwareNameSuffix();
	CCST_BlockBreak_OnSurvivalStateMayHaveChanged();
}

static void CCST_OnMapLoaded(void* obj) {
	(void)obj;
	CCST_Policy_Refresh();
	CCST_Motd_OnHackPermsChanged(NULL);
	CCST_Health_Reset();
	CCST_Score_Reset();
	CCST_BlockBreak_OnMapLoaded();
}

static void CCST_OnHacksChanged(void* obj) {
	(void)obj;
	CCST_Health_EnforceSurvivalHacks();
}

static void CCST_OnContextLost(void* obj) {
	(void)obj;
	CCST_Icons_ContextLost();
	CCST_Inv_ContextLost();
	CCST_Deathscreen_ContextLost();
}

static void CCST_RegisterEvents(void) {
	Event_Register_(&NetEvents.Connected, NULL, CCST_OnConnected);
	Event_Register_(&WorldEvents.MapLoaded, NULL, CCST_OnMapLoaded);
	Event_Register_(&UserEvents.HacksStateChanged, NULL, CCST_OnHacksChanged);
	Event_Register_(&GfxEvents.ContextLost, NULL, CCST_OnContextLost);
}

static void CCST_UnregisterEvents(void) {
	Event_Unregister_(&NetEvents.Connected, NULL, CCST_OnConnected);
	Event_Unregister_(&WorldEvents.MapLoaded, NULL, CCST_OnMapLoaded);
	Event_Unregister_(&UserEvents.HacksStateChanged, NULL, CCST_OnHacksChanged);
	Event_Unregister_(&GfxEvents.ContextLost, NULL, CCST_OnContextLost);
}

static void CCST_PluginInit(void) {
	CCST_Policy_Init();
	CCST_Motd_Init();
	CCST_Health_Init();
	CCST_BlockBreak_Init();
	CCST_TextureNoise_Init();
	CCST_BlockValue_Init();
	CCST_Tilefood_Init();
	CCST_Inv_Init();
	CCST_Score_Init();
	CCST_Deathscreen_Init();
	CCST_Netshim_Init();
	CCST_Peer_Init();
	CCST_Persist_Init();
	CCST_Policy_Refresh();
	CCST_AppendSoftwareNameSuffix();

	ScheduledTask_Add(GAME_DEF_TICKS, CCST_Health_OnTick);
	ScheduledTask_Add(GAME_DEF_TICKS, CCST_Persist_OnTick);

	Game.Draw2DHooks[1] = CCST_Icons_Draw2D;
	Game.Draw2DHooks[2] = CCST_BlockBreak_Draw2D;

	CCST_RegisterEvents();

	Commands_Register(&gamemode_cmd);
	Commands_Register(&inv_cmd);
	Commands_Register(&ccst_cmd);
}

static void CCST_PluginFree(void) {
	Game.Draw2DHooks[1] = NULL;
	Game.Draw2DHooks[2] = NULL;

	CCST_UnregisterEvents();

	CCST_Icons_Free();
	CCST_BlockBreak_Free();
	CCST_Deathscreen_Free();
	CCST_Inv_Free();
	CCST_Tilefood_Free();
	CCST_BlockValue_Free();
	CCST_TextureNoise_Free();
	CCST_Score_Free();
	CCST_Peer_Free();
	CCST_Netshim_Free();
	CCST_Persist_Free();
	CCST_Motd_Free();
	CCST_Health_Free();
}

static void OnInit(void) {
	CCST_PluginInit();
}

static void OnFree(void) {
	CCST_PluginFree();
}

static void OnReset(void) {
	CCST_UnregisterEvents();
	CCST_RegisterEvents();
	if (Entities.CurPlayer)
		CCST_Motd_OnHackPermsChanged(NULL);
	else
		CCST_Motd_Reset();
	CCST_Policy_Refresh();
	CCST_Health_Reset();
	CCST_Score_Reset();
	CCST_Inv_ClearAll();
	CCST_BlockBreak_OnSurvivalStateMayHaveChanged();
	CCST_Netshim_Refresh();
	CCST_Peer_Refresh();
}

static void OnNewMap(void) { }

static void OnNewMapLoaded(void) {
	CCST_UnregisterEvents();
	CCST_RegisterEvents();
	CCST_Policy_Refresh();
	CCST_Motd_OnHackPermsChanged(NULL);
	CCST_Health_Reset();
	CCST_Score_Reset();
	CCST_Inv_OnMapBegun();
	CCST_BlockBreak_OnMapLoaded();
	CCST_Persist_OnNewMapLoaded();
}

PLUGIN_EXPORT int Plugin_ApiVersion = GAME_API_VER;
PLUGIN_EXPORT struct IGameComponent Plugin_Component = {
	OnInit,
	OnFree,
	OnReset,
	OnNewMap,
	OnNewMapLoaded,
	NULL
};
