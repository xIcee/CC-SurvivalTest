#ifndef CCST_MOTD_H
#define CCST_MOTD_H
#include "Core.h"

typedef enum {
	CCST_MOTD_GM_NONE = 0,
	CCST_MOTD_GM_CREATIVE,
	CCST_MOTD_GM_SURVIVAL
} CCST_MotdForcedGamemode;

typedef enum {
	CCST_MOTD_TOGGLE_AUTO = 0,
	CCST_MOTD_TOGGLE_FORCE_OFF,
	CCST_MOTD_TOGGLE_FORCE_ON
} CCST_MotdToggle;

void CCST_Motd_Init(void);
void CCST_Motd_Free(void);
/* Clears MOTD-derived state (call when disconnecting / plugin reset). */
void CCST_Motd_Reset(void);
/* Re-read HacksFlags (Server.Name + Server.MOTD, same as WoM-style flags). Safe when CurPlayer is NULL. */
void CCST_Motd_OnHackPermsChanged(void* obj);

CCST_MotdForcedGamemode CCST_Motd_GetForcedGamemode(void);
cc_bool CCST_Motd_IsGodMode(void);

cc_bool CCST_Motd_HasSurvivalToken(void);

CCST_MotdToggle CCST_Motd_Mining(void);   /* +mine / -mine */
cc_bool CCST_Motd_InvDisabled(void);      /* -inv */
CCST_MotdToggle CCST_Motd_Heal(void);     /* +heal / -heal */
CCST_MotdToggle CCST_Motd_Combat(void);   /* +combat / -combat */
cc_bool CCST_Motd_DeathMsgDisabled(void); /* -deathmsg */

#endif
