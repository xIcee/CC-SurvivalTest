/*
 * Death chat lines: strings match MCP-919 assets/minecraft/lang/en_US.lang (death.attack.*, death.*).
 */
#include "deathmsg.h"
#include "Constants.h"
#include "Entity.h"
#include "Chat.h"
#include "Options.h"
#include "String_.h"
#include "peer.h"
#include "policy.h"

/* Game_Username is not CC_VAR, TabList self + Options username (see ccst_api.h). */
static void ccst_death_append_local_victim(cc_string* out) {
	if (TabList.NameOffsets[ENTITIES_SELF_ID]) {
		cc_string t = TabList_UNSAFE_GetPlayer(ENTITIES_SELF_ID);
		String_AppendColorless(out, &t);
		return;
	}
	Options_Get(LOPT_USERNAME, out, DEFAULT_USERNAME);
}

static CCST_DeathCause s_pending_cause = CCST_DEATH_CAUSE_GENERIC;
static int s_pending_inciter = -1;

void CCST_DeathMsg_SetNextCause(CCST_DeathCause cause, int inciterEntityId) {
	s_pending_cause  = cause;
	s_pending_inciter = inciterEntityId;
}

void CCST_DeathMsg_ConsumePending(CCST_DeathCause* cause, int* inciterEntityId) {
	if (cause) *cause = s_pending_cause;
	if (inciterEntityId) *inciterEntityId = s_pending_inciter;
	s_pending_cause  = CCST_DEATH_CAUSE_GENERIC;
	s_pending_inciter = -1;
}

void CCST_DeathMsg_AbortPending(void) {
	s_pending_cause  = CCST_DEATH_CAUSE_GENERIC;
	s_pending_inciter = -1;
}

static void ccst_death_append_entity_name(int entityId, cc_string* out) {
	struct Entity* e;
	cc_string raw;

	if (entityId < 0 || entityId >= ENTITIES_MAX_COUNT) return;
	if (TabList.NameOffsets[entityId]) {
		raw = TabList_UNSAFE_GetPlayer(entityId);
		String_AppendColorless(out, &raw);
		return;
	}
	e = Entities.List[entityId];
	if (e && e->NameRaw[0]) {
		int len = String_CalcLen(e->NameRaw, STRING_SIZE);
		raw = String_Init(e->NameRaw, len, STRING_SIZE);
		String_AppendColorless(out, &raw);
	}
}

static void ccst_death_fill_victim(int victimEntityId, cc_string* out) {
	/* Self-slot or any slot resolving the local username fallback when the TabList/Entity lookup
	 * misses (some servers don't publish a self TabList entry). */
	if (victimEntityId == ENTITIES_SELF_ID) {
		ccst_death_append_local_victim(out);
		return;
	}
	ccst_death_append_entity_name(victimEntityId, out);
	if (out->length == 0) ccst_death_append_local_victim(out);
}

static cc_bool ccst_death_has_inciter_name(int entityId) {
	cc_string tmp; char buf[STRING_SIZE];
	String_InitArray(tmp, buf);
	ccst_death_append_entity_name(entityId, &tmp);
	return tmp.length > 0;
}

void CCST_DeathMsg_ShowFor(int victimEntityId, CCST_DeathCause cause, int inciterEntityId) {
	cc_string victim;      char victimBuf[STRING_SIZE];
	cc_string inciterName; char inciterBuf[STRING_SIZE];

	String_InitArray(victim, victimBuf);
	ccst_death_fill_victim(victimEntityId, &victim);
	String_InitArray(inciterName, inciterBuf);
	if (inciterEntityId >= 0)
		ccst_death_append_entity_name(inciterEntityId, &inciterName);

	/* &7 = gray, vanilla-style system text; %1$s = victim display name. */
	switch (cause) {
	case CCST_DEATH_CAUSE_LAVA:
		if (inciterEntityId >= 0 && ccst_death_has_inciter_name(inciterEntityId))
			Chat_Add2("&7%s tried to swim in lava to escape %s", &victim, &inciterName);
		else
			Chat_Add1("&7%s tried to swim in lava", &victim);
		break;
	case CCST_DEATH_CAUSE_DROWN:
		if (inciterEntityId >= 0 && ccst_death_has_inciter_name(inciterEntityId))
			Chat_Add2("&7%s drowned whilst trying to escape %s", &victim, &inciterName);
		else
			Chat_Add1("&7%s drowned", &victim);
		break;
	case CCST_DEATH_CAUSE_FALL:
		/* death.attack.fall / death.fell.assist (MCP-919 CombatTracker fall messaging). */
		if (inciterEntityId >= 0 && ccst_death_has_inciter_name(inciterEntityId))
			Chat_Add2("&7%s was doomed to fall by %s", &victim, &inciterName);
		else
			Chat_Add1("&7%s hit the ground too hard", &victim);
		break;
	case CCST_DEATH_CAUSE_MOB:
		if (inciterName.length)
			Chat_Add2("&7%s was slain by %s", &victim, &inciterName);
		else
			Chat_Add1("&7%s died", &victim);
		break;
	case CCST_DEATH_CAUSE_PLAYER:
		if (inciterName.length)
			Chat_Add2("&7%s was slain by %s", &victim, &inciterName);
		else
			Chat_Add1("&7%s died", &victim);
		break;
	case CCST_DEATH_CAUSE_POISON:
		Chat_Add1("&7%s was killed by magic", &victim);
		break;
	case CCST_DEATH_CAUSE_OUT_OF_WORLD:
		Chat_Add1("&7%s fell out of the world", &victim);
		break;
	case CCST_DEATH_CAUSE_GENERIC:
	default:
		Chat_Add1("&7%s died", &victim);
		break;
	}
}

void CCST_DeathMsg_Show(CCST_DeathCause cause, int inciterEntityId) {
	CCST_DeathMsg_ShowFor(ENTITIES_SELF_ID, cause, inciterEntityId);
}

void CCST_DeathMsg_ShowAndTransmit(CCST_DeathCause cause, int inciterEntityId) {
	CCST_DeathMsg_Show(cause, inciterEntityId);
	if (CCST_Policy_DeathMsgStreamEnabled()) {
		/* Peer layer handles single-player gating. */
		CCST_Peer_SendDeath((int)cause, inciterEntityId);
	}
}
