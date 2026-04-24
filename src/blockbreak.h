#ifndef CCST_BLOCKBREAK_H
#define CCST_BLOCKBREAK_H
#include "Core.h"

void CCST_BlockBreak_Init(void);
void CCST_BlockBreak_Free(void);
void CCST_BlockBreak_OnMapLoaded(void);
void CCST_BlockBreak_OnSurvivalStateMayHaveChanged(void);
void CCST_BlockBreak_OnSetBlockPermission(BlockID block, cc_bool canDelete);
void CCST_BlockBreak_Draw2D(float delta);

#endif
