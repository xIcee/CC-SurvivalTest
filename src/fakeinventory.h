#ifndef CCST_FAKEINVENTORY_H
#define CCST_FAKEINVENTORY_H
#include "BlockID.h"
#include "Core.h"

#define CCST_INV_SLOTS 36 /* 9 hotbar + 27 main; separate transmute slot + cursor in fakeinventory.c */
#define CCST_INV_STACK_MAX 64

cc_bool CCST_Inv_IsOpen(void);
/* When opening, any vanilla inventory screen is closed first (see CCST_Inv_OpenInternal). */
void CCST_Inv_SetOpen(cc_bool open);
void CCST_Inv_Draw2D(float delta);
void CCST_Inv_Init(void);
void CCST_Inv_ContextLost(void);
void CCST_Inv_Free(void);

/* Survival layer: empty all hotbar rows and plugin storage when a world loads.
 * CPE SetHotbar from the server is applied after map load in normal packet order, so
 * an organized server hotbar still appears once those packets are processed.
 * Inventory.Map (vanilla block-picker ordering) is not reset here, Game_Version is not plugin-exported. */
void CCST_Inv_OnMapBegun(void);
/* Clear hotbar table, plugin slots, transmute, and cursor (used on death / explicit reset). */
void CCST_Inv_ClearAll(void);

/* Add one mined block into survival stacks (merge to 64, then empty slot). Returns true if stored. */
cc_bool CCST_Inv_TryPickupMinedBlock(BlockID block);
/* Draw stack counts (>1) over the HUD hotbar; call from Draw2D after the hotbar is drawn. */
void CCST_Inv_DrawHotbarStackCounts2D(void);
/* Survival: consume one item from the selected hotbar stack if it matches expect. */
cc_bool CCST_Inv_TryConsumeOneSelected(BlockID expect);

/* Called after a server CPE SetHotbar packet is applied to Inventory. */
void CCST_Inv_OnServerSetHotbar(int index, BlockID block);

/* Persistence */
void CCST_Inv_GetState(cc_uint16 blocks[CCST_INV_SLOTS], cc_uint8 counts[CCST_INV_SLOTS],
	cc_uint16* transmuteBlock, cc_uint8* transmuteCount);
void CCST_Inv_SetState(const cc_uint16 blocks[CCST_INV_SLOTS], const cc_uint8 counts[CCST_INV_SLOTS],
	BlockID transmuteBlock, int transmuteCount);

#endif
