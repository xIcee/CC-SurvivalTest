/*
    motd.c
    server policy parsing via motd tokens.

    - forces gamemodes (+survival, +creative)
    - toggles mining, healing, and combat
    - reacts to hack permission changes
*/
#include "Entity.h"
#include "Event.h"
#include "Server.h"
#include "String_.h"
#include "ccst_api.h"
#include "blockbreak.h"
#include "health.h"
#include "motd.h"
#include "policy.h"

static CCST_MotdForcedGamemode ccst_motd_gamemode;
static cc_bool ccst_motd_god;
static cc_bool ccst_motd_has_survival_token;
static CCST_MotdToggle ccst_motd_mining;
static cc_bool ccst_motd_inv_disabled;
static CCST_MotdToggle ccst_motd_heal;
static CCST_MotdToggle ccst_motd_combat;
static cc_bool ccst_motd_deathmsg_disabled;

// parse survival flags from a string
static void CCST_Motd_ParseFromString(const cc_string* s) {
	static const cc_string tok_god       = String_FromConst("+god");
	static const cc_string tok_creative  = String_FromConst("+creative");
	static const cc_string tok_survival  = String_FromConst("+survival");
	static const cc_string tok_mine_on   = String_FromConst("+mine");
	static const cc_string tok_mine_off  = String_FromConst("-mine");
	static const cc_string tok_inv_off   = String_FromConst("-inv");
	static const cc_string tok_heal_on   = String_FromConst("+heal");
	static const cc_string tok_heal_off  = String_FromConst("-heal");
	static const cc_string tok_combat_on  = String_FromConst("+combat");
	static const cc_string tok_combat_off = String_FromConst("-combat");
	static const cc_string tok_deathmsg_off = String_FromConst("-deathmsg");
	static const cc_string tok_survival_off = String_FromConst("-survival");
	static const cc_string tok_creative_off = String_FromConst("-creative");

	ccst_motd_gamemode = CCST_MOTD_GM_NONE;
	ccst_motd_god      = false;
	ccst_motd_has_survival_token = false;
	ccst_motd_mining = CCST_MOTD_TOGGLE_AUTO;
	ccst_motd_inv_disabled = false;
	ccst_motd_heal = CCST_MOTD_TOGGLE_AUTO;
	ccst_motd_combat = CCST_MOTD_TOGGLE_AUTO;
	ccst_motd_deathmsg_disabled = false;

	if (!s || !s->length) return;

	if (String_CaselessContains(s, &tok_god))
		ccst_motd_god = true;

	// gamemode forcing
	if (String_CaselessContains(s, &tok_creative) || String_CaselessContains(s, &tok_survival_off)) {
		ccst_motd_gamemode = CCST_MOTD_GM_CREATIVE;
	} else if (String_CaselessContains(s, &tok_survival) || String_CaselessContains(s, &tok_creative_off)) {
		ccst_motd_gamemode = CCST_MOTD_GM_SURVIVAL;
	}

	if (String_CaselessContains(s, &tok_survival))
		ccst_motd_has_survival_token = true;

	// feature toggles
	if (String_CaselessContains(s, &tok_mine_on))
		ccst_motd_mining = CCST_MOTD_TOGGLE_FORCE_ON;
	else if (String_CaselessContains(s, &tok_mine_off))
		ccst_motd_mining = CCST_MOTD_TOGGLE_FORCE_OFF;

	if (String_CaselessContains(s, &tok_inv_off))
		ccst_motd_inv_disabled = true;

	if (String_CaselessContains(s, &tok_heal_on))
		ccst_motd_heal = CCST_MOTD_TOGGLE_FORCE_ON;
	else if (String_CaselessContains(s, &tok_heal_off))
		ccst_motd_heal = CCST_MOTD_TOGGLE_FORCE_OFF;

	if (String_CaselessContains(s, &tok_combat_on))
		ccst_motd_combat = CCST_MOTD_TOGGLE_FORCE_ON;
	else if (String_CaselessContains(s, &tok_combat_off))
		ccst_motd_combat = CCST_MOTD_TOGGLE_FORCE_OFF;

	if (String_CaselessContains(s, &tok_deathmsg_off))
		ccst_motd_deathmsg_disabled = true;
}

void CCST_Motd_Reset(void) {
	ccst_motd_gamemode = CCST_MOTD_GM_NONE;
	ccst_motd_god      = false;
	ccst_motd_has_survival_token = false;
	ccst_motd_mining = CCST_MOTD_TOGGLE_AUTO;
	ccst_motd_inv_disabled = false;
	ccst_motd_heal = CCST_MOTD_TOGGLE_AUTO;
	ccst_motd_combat = CCST_MOTD_TOGGLE_AUTO;
	ccst_motd_deathmsg_disabled = false;
}

CCST_MotdForcedGamemode CCST_Motd_GetForcedGamemode(void) {
	return ccst_motd_gamemode;
}

cc_bool CCST_Motd_IsGodMode(void) {
	return ccst_motd_god;
}

cc_bool CCST_Motd_HasSurvivalToken(void) { return ccst_motd_has_survival_token; }
CCST_MotdToggle CCST_Motd_Mining(void) { return ccst_motd_mining; }
cc_bool CCST_Motd_InvDisabled(void) { return ccst_motd_inv_disabled; }
CCST_MotdToggle CCST_Motd_Heal(void) { return ccst_motd_heal; }
CCST_MotdToggle CCST_Motd_Combat(void) { return ccst_motd_combat; }
cc_bool CCST_Motd_DeathMsgDisabled(void) { return ccst_motd_deathmsg_disabled; }

void CCST_Motd_OnHackPermsChanged(void* obj) {
	struct LocalPlayer* p;
	const cc_string* s;

	(void)obj;
	if (!CCST_Policy_PluginEnabled()) return;
	p = Entities.CurPlayer;
	if (!p) return;

	s = &p->Hacks.HacksFlags;

	CCST_Motd_ParseFromString(s);
	CCST_Policy_Refresh();
	CCST_BlockBreak_OnSurvivalStateMayHaveChanged();
	CCST_Health_EnforceSurvivalHacks();
}

void CCST_Motd_Init(void) {
	CCST_Motd_Reset();
	Event_Register_(&UserEvents.HackPermsChanged, NULL, CCST_Motd_OnHackPermsChanged);
}

void CCST_Motd_Free(void) {
	Event_Unregister_(&UserEvents.HackPermsChanged, NULL, CCST_Motd_OnHackPermsChanged);
}
