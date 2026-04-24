#ifndef CCST_DEATHMSG_H
#define CCST_DEATHMSG_H
#include "Core.h"

/*
 * Local death chat lines: mirror MCP-919 en_US.lang death.* / death.attack.* patterns (%1$s = victim).
 * Inciter is an entity id when relevant (TabList / NameRaw), or -1.
 */
typedef enum CCST_DeathCause {
	CCST_DEATH_CAUSE_GENERIC = 0,
	CCST_DEATH_CAUSE_LAVA,
	CCST_DEATH_CAUSE_DROWN,
	CCST_DEATH_CAUSE_FALL,
	CCST_DEATH_CAUSE_MOB,
	CCST_DEATH_CAUSE_PLAYER,
	CCST_DEATH_CAUSE_POISON,
	CCST_DEATH_CAUSE_OUT_OF_WORLD
} CCST_DeathCause;

void CCST_DeathMsg_SetNextCause(CCST_DeathCause cause, int inciterEntityId);
void CCST_DeathMsg_ConsumePending(CCST_DeathCause* cause, int* inciterEntityId);
void CCST_DeathMsg_AbortPending(void);

void CCST_DeathMsg_Show(CCST_DeathCause cause, int inciterEntityId);

void CCST_DeathMsg_ShowFor(int victimEntityId, CCST_DeathCause cause, int inciterEntityId);
void CCST_DeathMsg_ShowAndTransmit(CCST_DeathCause cause, int inciterEntityId);

#endif
