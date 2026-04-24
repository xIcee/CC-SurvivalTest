#ifndef CCST_BLOCKVALUE_H
#define CCST_BLOCKVALUE_H
#include "BlockID.h"
#include "Core.h"

void CCST_BlockValue_Init(void);
void CCST_BlockValue_Free(void);

float CCST_BlockValue_Stuff(BlockID b);

float CCST_BlockValue_Luxury(BlockID b);

void CCST_BlockValue_MeanHueSat(BlockID b, float* out_hue_deg, float* out_sat);

#endif
