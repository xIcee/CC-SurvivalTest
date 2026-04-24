#ifndef CCST_SCORE_H
#define CCST_SCORE_H
#include "BlockID.h"
#include "Core.h"

void CCST_Score_Init(void);
void CCST_Score_Free(void);
void CCST_Score_Reset(void);
void CCST_Score_Add(int delta);
int CCST_Score_Get(void);
void CCST_Score_Set(int score);
/* First time this block *family* appears in survival inventory (AutoRotate group; per life / until reset). */
void CCST_Score_TryDiscover(BlockID b);

#endif
