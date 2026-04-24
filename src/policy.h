#ifndef CCST_POLICY_H
#define CCST_POLICY_H
#include "Core.h"

void CCST_Policy_Init(void);
/* Recompute effective policy from server tokens (HacksFlags/MOTD) and apply forced state. */
void CCST_Policy_Refresh(void);
cc_bool CCST_Policy_PluginEnabled(void);
cc_bool CCST_Policy_SurvivalMechanics(void);

/* True when the server forces +survival/-creative or +creative/-survival. */
cc_bool CCST_Policy_IsGamemodeForced(void);
/* Whether the /gamemode command is allowed in the current level. */
cc_bool CCST_Policy_AllowGamemodeCommand(void);

/* Mining enablement:
 * - +mine: always mine (even in creative)
 * - -mine: never mine (even in survival)
 * - default: mine only in survival */
cc_bool CCST_Policy_MiningEnabled(void);

/* Survival inventory availability. */
cc_bool CCST_Policy_SurvivalInventoryEnabled(void);

/* Tile food consumption availability. */
cc_bool CCST_Policy_TileFoodEnabled(void);

/* Combat (peer melee) availability. */
cc_bool CCST_Policy_CombatEnabled(void);

/* Death message stream availability (peer send/receive). */
cc_bool CCST_Policy_DeathMsgStreamEnabled(void);

#endif
