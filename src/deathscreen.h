#ifndef CCST_DEATHSCREEN_H
#define CCST_DEATHSCREEN_H
#include "Core.h"

void CCST_Deathscreen_Init(void);
void CCST_Deathscreen_ContextLost(void);
void CCST_Deathscreen_Free(void);
cc_bool CCST_Deathscreen_IsActive(void);

void CCST_Deathscreen_Begin(int finalScore);
void CCST_Deathscreen_Draw2D(float delta);

void CCST_Deathscreen_ForceCancel(void);

#endif
