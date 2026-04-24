#include "blockfamily.h"
#include "Block.h"
#include "String_.h"
#include "Funcs.h"
#include <string.h>

// same grouping as ClassiCube Block.c AutoRotate_BlocksShareGroup
#define CCST_AR_GROUP_CORNERS    0
#define CCST_AR_GROUP_VERTICAL   1
#define CCST_AR_GROUP_DIRECTION  2
#define CCST_AR_GROUP_PILLAR     3
#define CCST_AR_EQ1(x)    (dir0 == (x) && dir1 == '\0')
#define CCST_AR_EQ2(x, y) (dir0 == (x) && dir1 == (y))

static int CCST_AR_CalcGroup(const cc_string* dir) {
	char dir0, dir1;
	dir0 = dir->length > 1 ? dir->buffer[1] : '\0'; Char_MakeLower(dir0);
	dir1 = dir->length > 2 ? dir->buffer[2] : '\0'; Char_MakeLower(dir1);

	if (CCST_AR_EQ2('n','w') || CCST_AR_EQ2('n','e') || CCST_AR_EQ2('s','w') || CCST_AR_EQ2('s','e')) {
		return CCST_AR_GROUP_CORNERS;
	} else if (CCST_AR_EQ1('u') || CCST_AR_EQ1('d')) {
		return CCST_AR_GROUP_VERTICAL;
	} else if (CCST_AR_EQ1('n') || CCST_AR_EQ1('w') || CCST_AR_EQ1('s') || CCST_AR_EQ1('e')) {
		return CCST_AR_GROUP_DIRECTION;
	} else if (CCST_AR_EQ2('u','d') || CCST_AR_EQ2('w','e') || CCST_AR_EQ2('n','s')) {
		return CCST_AR_GROUP_PILLAR;
	}
	return -1;
}

static void CCST_GetAutoRotateTypes(cc_string* name, int* dirTypes) {
	int dirIndex, i;
	cc_string dir;
	dirTypes[0] = -1;
	dirTypes[1] = -1;

	for (i = 0; i < 2; i++) {
		dirIndex = String_LastIndexOf(name, '-');
		if (dirIndex == -1) return;

		dir = String_UNSAFE_SubstringAt(name, dirIndex);
		dirTypes[i] = CCST_AR_CalcGroup(&dir);
		name->length = dirIndex;
	}
}

cc_bool CCST_BlockFamily_ShareGroup(BlockID block, BlockID other) {
	cc_string bName, oName;
	int bDirTypes[2], oDirTypes[2];

	bName = Block_UNSAFE_GetName(block);
	CCST_GetAutoRotateTypes(&bName, bDirTypes);
	if (bDirTypes[0] == -1) return false;

	oName = Block_UNSAFE_GetName(other);
	CCST_GetAutoRotateTypes(&oName, oDirTypes);
	if (oDirTypes[0] == -1) return false;

	return bDirTypes[0] == oDirTypes[0] && bDirTypes[1] == oDirTypes[1]
		&& String_CaselessEquals(&bName, &oName);
}

static BlockID ccst_familyCanonMemo[BLOCK_COUNT];
static cc_uint8 ccst_familyCanonReady[BLOCK_COUNT];

BlockID CCST_BlockFamily_Canonical(BlockID b) {
	BlockID k, minId;
	if (b == BLOCK_AIR || b >= BLOCK_COUNT || Blocks.Draw[b] == DRAW_GAS) return BLOCK_AIR;
	if (ccst_familyCanonReady[b]) return ccst_familyCanonMemo[b];

	minId = b;
	for (k = 1; k < BLOCK_COUNT; k++) {
		if (k == b) continue;
		if (Blocks.Draw[k] == DRAW_GAS) continue;
		if (CCST_BlockFamily_ShareGroup(b, k) && k < minId)
			minId = k;
	}
	ccst_familyCanonMemo[b] = minId;
	ccst_familyCanonReady[b] = 1;
	return minId;
}

void CCST_BlockFamily_ClearMemo(void) {
	memset(ccst_familyCanonReady, 0, sizeof(ccst_familyCanonReady));
}
