/*
    policy.c
    centralized feature toggle logic.

    - survival mechanics enforcement
    - command permission checks
    - inventory and combat availability
*/
#include "policy.h"
#include "health.h"
#include "motd.h"

void CCST_Policy_Init(void) {
}

void CCST_Policy_Refresh(void) {
	switch (CCST_Motd_GetForcedGamemode()) {
	case CCST_MOTD_GM_SURVIVAL:
		CCST_Health_SetModeSurvival(true);
		break;
	case CCST_MOTD_GM_CREATIVE:
		CCST_Health_SetModeSurvival(false);
		break;
	default:
		break;
	}
}

cc_bool CCST_Policy_PluginEnabled(void) {
	return true;
}

cc_bool CCST_Policy_SurvivalMechanics(void) {
	return true;
}

cc_bool CCST_Policy_IsGamemodeForced(void) {
	return CCST_Motd_GetForcedGamemode() != CCST_MOTD_GM_NONE;
}

cc_bool CCST_Policy_AllowGamemodeCommand(void) {
	return !CCST_Policy_IsGamemodeForced();
}

cc_bool CCST_Policy_MiningEnabled(void) {
	CCST_MotdToggle t = CCST_Motd_Mining();
	if (!CCST_Policy_PluginEnabled()) return false;
	if (t == CCST_MOTD_TOGGLE_FORCE_ON)  return true;
	if (t == CCST_MOTD_TOGGLE_FORCE_OFF) return false;
	return CCST_Health_IsSurvivalActive();
}

cc_bool CCST_Policy_SurvivalInventoryEnabled(void) {
	if (!CCST_Policy_PluginEnabled()) return false;
	if (!CCST_Health_IsSurvivalActive()) return false;
	return !CCST_Motd_InvDisabled();
}

cc_bool CCST_Policy_TileFoodEnabled(void) {
	CCST_MotdToggle t = CCST_Motd_Heal();
	if (!CCST_Policy_PluginEnabled()) return false;
	if (t == CCST_MOTD_TOGGLE_FORCE_ON)  return true;
	if (t == CCST_MOTD_TOGGLE_FORCE_OFF) return false;
	return CCST_Health_IsSurvivalActive();
}

cc_bool CCST_Policy_CombatEnabled(void) {
	CCST_MotdToggle t = CCST_Motd_Combat();
	if (!CCST_Policy_PluginEnabled()) return false;
	if (t == CCST_MOTD_TOGGLE_FORCE_OFF) return false;
	return true;
}

cc_bool CCST_Policy_DeathMsgStreamEnabled(void) {
	if (!CCST_Policy_PluginEnabled()) return false;
	return !CCST_Motd_DeathMsgDisabled();
}
