#ifndef CCST_BLOCKTYPE_H
#define CCST_BLOCKTYPE_H
#include "BlockID.h"
#include "Core.h"

float CCST_BlockType_BreakSecondsForDigSound(cc_uint8 digSound);
float CCST_BlockType_BreakShapeMul(cc_uint8 draw);
float CCST_BlockType_BreakTimeFor(BlockID b);

// fluids (CPE extended collide), gas/air-like draws, and fire
cc_bool CCST_BlockType_IsTransmuteExcluded(BlockID b);

cc_bool CCST_BlockType_CanTransmute(BlockID from, BlockID to);

#define CCST_TRANSMUTE_MAX_SIDE 9
#define CCST_TRANSMUTE_MAX_OUT 64

cc_bool CCST_BlockType_TransmuteRatio(BlockID from, BlockID to, int* out_in, int* out_out);

void CCST_BlockType_TransmuteExchange(BlockID from, int count_from, BlockID to,
	int max_out_cap,
	int* out_produced, int* from_consumed, cc_bool* out_probabilistic);


cc_bool CCST_BlockType_TransmutePreview(BlockID from, int count_from, BlockID to,
	int max_out_cap,
	int* out_produced, int* out_consumed);

#endif
