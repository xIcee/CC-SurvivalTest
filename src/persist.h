#ifndef CCST_PERSIST_H
#define CCST_PERSIST_H
#include "Core.h"

struct ScheduledTask;

void CCST_Persist_Init(void);
void CCST_Persist_Free(void);

/* Mark the world environment as changed, triggering a recompute on the next save. */
void CCST_Persist_MarkDirty(void);

/* Call after CCST_Inv_OnMapBegun / CCST_Health_Reset / CCST_Score_Reset on each map load. */
void CCST_Persist_OnNewMapLoaded(void);

/* Periodic save tick (ScheduledTask_Add with GAME_DEF_TICKS). */
void CCST_Persist_OnTick(struct ScheduledTask* task);

/* True when a near-match candidate was found but not auto-restored due confidence rules. */
cc_bool CCST_Persist_HasSuggestedCandidate(void);
/* Force-apply the most likely candidate from the last restore attempt. */
cc_bool CCST_Persist_ForceSuggestedRestore(void);
/* Replace current world's persisted state with the current runtime state. */
cc_bool CCST_Persist_ResetCurrentWorldSave(void);

#endif

