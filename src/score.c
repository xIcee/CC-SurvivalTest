/*
    score.c
    survival discovery and scoring.

    - block discovery awards
*/
#include "PluginAPI.h"
#include "Block.h"
#include "BlockID.h"
#include "Core.h"
#include "blockfamily.h"
#include "blockvalue.h"
#include "health.h"
#include "policy.h"
#include "score.h"
#include <string.h>

#define CCST_SCORE_DISCOVER         8
#define CCST_SCORE_DISCOVER_LUX_MAX 22

static int ccst_score;
static cc_uint8 ccst_discovered[BLOCK_COUNT];

// award points for newly discovered block types
void CCST_Score_TryDiscover(BlockID b) {
	BlockID canon;
	float lux;
	int award;

	if (!CCST_Policy_PluginEnabled() || !CCST_Health_IsSurvivalActive()) return;
	if (CCST_Health_IsDeadOrDying()) return;
	if (b == BLOCK_AIR || b >= BLOCK_COUNT || Blocks.Draw[b] == DRAW_GAS) return;

	canon = CCST_BlockFamily_Canonical(b);
	if (canon == BLOCK_AIR || canon >= BLOCK_COUNT || Blocks.Draw[canon] == DRAW_GAS) return;
	if (ccst_discovered[canon]) return;
	ccst_discovered[canon] = 1;

	lux = CCST_BlockValue_Luxury(canon);
	if (lux < 0.0f) lux = 0.0f;
	if (lux > 1.0f) lux = 1.0f;
	award = CCST_SCORE_DISCOVER + (int)(lux * (float)CCST_SCORE_DISCOVER_LUX_MAX + 0.5f);
	CCST_Score_Add(award);
}

void CCST_Score_Init(void) {
	ccst_score = 0;
	memset(ccst_discovered, 0, sizeof(ccst_discovered));
	CCST_BlockFamily_ClearMemo();
}

void CCST_Score_Free(void) {
}

void CCST_Score_Reset(void) {
	ccst_score = 0;
	memset(ccst_discovered, 0, sizeof(ccst_discovered));
	CCST_BlockFamily_ClearMemo();
}

void CCST_Score_Add(int delta) {
	if (delta <= 0) return;
	if (ccst_score > 2000000000 - delta)
		ccst_score = 2000000000;
	else
		ccst_score += delta;
}

int CCST_Score_Get(void) {
	return ccst_score;
}

void CCST_Score_Set(int score) {
	if (score < 0) score = 0;
	if (score > 2000000000) score = 2000000000;
	ccst_score = score;
}
