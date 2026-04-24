#ifndef CCST_BLOCKFAMILY_H
#define CCST_BLOCKFAMILY_H
#include "BlockID.h"
#include "Core.h"

cc_bool CCST_BlockFamily_ShareGroup(BlockID a, BlockID b);
BlockID CCST_BlockFamily_Canonical(BlockID b);
void CCST_BlockFamily_ClearMemo(void);

#endif
