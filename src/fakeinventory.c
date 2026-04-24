/*
    fakeinventory.c
    survival inventory overlay and stack management.

    - beta 1.7.3 layout and click semantics
    - transmutation list with recipe previews
    - hotbar synchronization and persistence
*/
#include "PluginAPI.h"
#include "Chat.h"
#include "Block.h"
#include "BlockID.h"
#include "Constants.h"
#include "Core.h"
#include "Drawer.h"
#include "Drawer2D.h"
#include "Event.h"
#include "ExtMath.h"
#include "Funcs.h"
#include "Game.h"
#include "Graphics.h"
#include "Gui.h"
#include "Input.h"
#include "Inventory.h"
#include "Options.h"
#include "String_.h"
#include "TexturePack.h"
#include "Window.h"
#include <math.h>
#include <string.h>
#include "ccst_binds.h"
#include "ccst_ui_scale.h"
#include "ccst_grabscreen.h"
#include "ccst_font.h"
#include "ccst_draw2d.h"
#include "ccst_drawtext.h"
#include "Options.h"
#include "blocktype.h"
#include "fakeinventory.h"
#include "isoinv.h"
#include "health.h"
#include "policy.h"
#include "ccst_api.h"
#include "deathscreen.h"
#include "score.h"
#include "blockfamily.h"
#include "Gui.h"
#include "persist.h"

#define MC_PANEL_W 176
#define MC_PANEL_H 174
#define MC_STEP    18
#define MC_SLOT    16

// layout metrics
#define MC_GRID_Y   92
#define MC_HOTBAR_Y (MC_GRID_Y + 58)

/* Transmutation list is embedded inside the main panel (top-right dead space). */
#define MC_LIST_W      68
#define MC_LIST_ICON_W 16  /* left slot-sized icon column within a list row */
#define MC_LIST_MARG_X  8
#define MC_LIST_TOP     16
#define MC_LIST_BOTTOM  (MC_GRID_Y - 4) /* keep above main inventory row band */
#define MC_LIST_MCX    (MC_PANEL_W - MC_LIST_MARG_X - MC_LIST_W)
#define MC_FULL_W      (MC_PANEL_W)

/* Transmute ("crafting") slot position (MC px) */
#define MC_TRANSMUTE_X 44
#define MC_TRANSMUTE_CY ((MC_LIST_TOP + MC_LIST_BOTTOM) / 2)
#define MC_TRANSMUTE_Y  (MC_TRANSMUTE_CY - (MC_SLOT / 2))

#define CCST_SLOT_TRANSMUTE 36
/* Isometric blocks: up to 12 verts each (3 faces × 4); leave headroom for many slots + list + cursor. */
#define CCST_INV_ICON_MAX_VERTS 2048

static cc_bool ccst_inv_open;
static BlockID ccst_slots[CCST_INV_SLOTS];
static int ccst_slot_count[CCST_INV_SLOTS];
static BlockID ccst_transmute;
static BlockID ccst_cursor;
static int ccst_transmute_count;
static int ccst_cursor_count;

static GfxResourceID ccst_inv_vb;
static GfxResourceID ccst_flatVb;
static GfxResourceID ccst_labelQuadVb; /* text stack counts; Gfx_Draw2DTexture not CC_API */

static int ccst_inv_tex_state[CCST_INV_ICON_MAX_VERTS / 4];

static BlockID ccst_mut_blocks[BLOCK_COUNT];
static int ccst_mut_count;

static int ccst_list_scroll;
static int ccst_hover_kind; /* 0 none, 1 slot, 2 list */
static int ccst_hover_slot; /* 0..35 or CCST_SLOT_TRANSMUTE */
static int ccst_hover_list_row; /* visible row index 0..visible-1 */

static int ccst_layout_ox, ccst_layout_oy, ccst_layout_strideX, ccst_layout_strideY;
static float ccst_layout_iconHalf; /* TableWidget: normBlockSize from blockSize * 0.7/2 (Widgets.c) */

static struct FontDesc ccst_inv_countFont;
static int ccst_inv_countFontPt = -1; /* -1 = no Font_Make yet; else last point passed to Font_Make */
static struct FontDesc ccst_inv_titleFont;
static int ccst_inv_titleFontPt = -1;
static struct FontDesc ccst_inv_headingFont;
static int ccst_inv_headingFontPt = -1;

struct CCST_ListScrollbar {
	int x, y, width, height;
	int topRow, rowsTotal, rowsVisible;
	float scrollingAcc;
	int dragOffset;
	int draggingId; /* -1 = none, otherwise pointer id */
	int padding;
	int borderX, borderY;
	int nubsWidth, offsets[3];
};
static struct CCST_ListScrollbar ccst_inv_listScroll;
static int ccst_inv_listScrollW = -1; /* pixel width */

/* Invisible screen: grabsInput for correct cursor + blocks HUD hotbar wheel. */
static struct Screen ccst_invGrabScreen;

static void CCST_InvGrab_InitOnce(void) {
	CCST_GrabScreen_InitOnce(&ccst_invGrabScreen);
}

/* Same colours as TableWidget_Render2 (Widgets.c), InventoryScreen block table. */
#define CCST_TABLE_GRAD_TOP PackedCol_Make(34, 34, 34, 168)
#define CCST_TABLE_GRAD_BOT PackedCol_Make(57, 57, 104, 202)
#define CCST_SEL_GRAD_TOP   PackedCol_Make(255, 255, 255, 142)
#define CCST_SEL_GRAD_BOT   PackedCol_Make(255, 255, 255, 192)

/* Mirrors Gfx_Draw2DGradient / Gfx_Build2DGradient (_GraphicsBase.h); not CC_API. */
/* Implemented by CCST_Draw2D_Gradient (ccst_draw2d.h). */

/* See ccst_ui_scale.h for shared scale helpers. */

/* Beta 1.7.3 default item font ~8px in a 16px slot; scale point size with slot height. */
static int CCST_Inv_ClampCountFontPt(int pt) {
	if (pt < 8) return 8;
	if (pt > 32) return 32;
	return pt;
}

static void CCST_Inv_EnsureCountFontForPoint(int point) {
	CCST_Font_EnsurePoint(&ccst_inv_countFont, &ccst_inv_countFontPt,
		point, 8, 32, FONT_FLAGS_PADDING);
}

static void CCST_Inv_EnsureTitleFont(void) {
	/* Match InventoryScreen title: Gui_MakeBodyFont = Font_Make(16, none). */
	CCST_Font_EnsurePoint(&ccst_inv_titleFont, &ccst_inv_titleFontPt,
		16, 16, 16, FONT_FLAGS_NONE);
}

static void CCST_Inv_EnsureHeadingFont(void) {
	/* Keep headings consistent with picker too (body font, slightly dim color). */
	CCST_Font_EnsurePoint(&ccst_inv_headingFont, &ccst_inv_headingFontPt,
		16, 16, 16, FONT_FLAGS_NONE);
}

/*
 * Beta 1.7.3 net.minecraft.src.RenderItem.renderItemOverlayIntoGUI:
 *   drawStringWithShadow(s, slotLeft + 19 - 2 - strW, slotTop + 6 + 3, ...)
 * i.e. horizontal: left = slotLeft + 17 - strW. Vertical: beta passes y = slotTop + 9
 * (FontRenderer uses that as the text line position; Drawer2D textures are top-left
 * of the bitmap with shadow padding, so we use a slightly smaller offset than 9/16·slotH).
 */
static void CCST_Inv_DrawStackCountInItemSlot(int count, int slotLeft, int slotTop, int slotW, int slotH) {
	cc_string str;
	char buf[8];
	struct DrawTextArgs args;
	int fontPt;
	int py, rightX;

	if (count <= 1) return;
	if (Gfx.LostContext) return;
	if (slotW < 1 || slotH < 1) return;

	fontPt = CCST_Inv_ClampCountFontPt((slotH * 8 + 8) / 16);
	CCST_Inv_EnsureCountFontForPoint(fontPt);

	String_InitArray(str, buf);
	String_AppendInt(&str, count);
	args.text      = str;
	args.font      = &ccst_inv_countFont;
	args.useShadow = true;

	/* ~7/16 vs beta 9/16: nudge up a couple pixels so digits sit like vanilla */
	py = slotTop + (slotH * 7 + 8) / 16;
	rightX = slotLeft + (slotW * 17 + 8) / 16;
	CCST_Draw2D_TextRightAligned(&ccst_labelQuadVb, &args, rightX, py, PACKEDCOL_WHITE);
}

/* Generic per-row label (recipe "m:n" / yield "->K"). Reuses the count font so sizing is
   consistent with the in-slot stack-count numbers, and takes an explicit colour so
   unusable rows can be dimmed. */
static void CCST_Inv_DrawLabelAt(const cc_string* str, int px, int py, int fontPt, PackedCol col) {
	struct DrawTextArgs args;

	if (!str || str->length == 0) return;
	if (Gfx.LostContext) return;

	CCST_Inv_EnsureCountFontForPoint(fontPt);
	args.text      = *str;
	args.font      = &ccst_inv_countFont;
	args.useShadow = true;
	CCST_Draw2D_Text(&ccst_labelQuadVb, &args, px, py, col);
}

/* Forward declarations for coordinate transforms (defined later). */
static int CCST_Inv_ScrX(int mcX);
static int CCST_Inv_ScrY(int mcY);
static int CCST_Inv_ListVisibleRows(void);

static void CCST_Inv_DrawHoveredBlockTitle(void) {
	cc_string desc; char descBuf[STRING_SIZE * 2];
	cc_string name;
	BlockID blk;
	int id;
	struct DrawTextArgs args;

	if (Gfx.LostContext) return;

	blk = BLOCK_AIR;
	if (ccst_hover_kind == 1 && ccst_hover_slot >= 0) {
		if (ccst_hover_slot == CCST_SLOT_TRANSMUTE) blk = ccst_transmute;
		else if (ccst_hover_slot < CCST_INV_SLOTS)  blk = ccst_slots[ccst_hover_slot];
	} else if (ccst_hover_kind == 2 && ccst_hover_list_row >= 0) {
		int idx = ccst_list_scroll + ccst_hover_list_row;
		if (idx >= 0 && idx < ccst_mut_count) blk = ccst_mut_blocks[idx];
	}

	if (blk == BLOCK_AIR || blk >= BLOCK_COUNT || Blocks.Draw[blk] == DRAW_GAS) return;

	String_InitArray(desc, descBuf);
	name = Block_UNSAFE_GetName(blk);
	String_AppendString(&desc, &name);
	id = (int)blk;
	String_Format1(&desc, " (ID: %i)", &id);

	CCST_Inv_EnsureTitleFont();
	args.text      = desc;
	args.font      = &ccst_inv_titleFont;
	args.useShadow = true;
	CCST_Draw2D_TextCenteredX(&ccst_labelQuadVb, &args, Game.Width / 2, CCST_Inv_ScrY(6), PACKEDCOL_WHITE);
}

static cc_bool CCST_Inv_SameItemStack(BlockID a, BlockID b) {
	if (a == b) return true;
	return CCST_BlockFamily_ShareGroup(a, b);
}

/* Award score for any block types newly present after pickup / merge / swap / transmute. */
static void CCST_Inv_RefreshDiscovery(void) {
	int i;
	if (!CCST_Policy_PluginEnabled() || !CCST_Health_IsSurvivalActive()) return;
	for (i = 0; i < CCST_INV_SLOTS; i++) {
		if (ccst_slots[i] == BLOCK_AIR || ccst_slot_count[i] <= 0) continue;
		CCST_Score_TryDiscover(ccst_slots[i]);
	}
	if (ccst_transmute != BLOCK_AIR && ccst_transmute_count > 0)
		CCST_Score_TryDiscover(ccst_transmute);
	if (ccst_cursor != BLOCK_AIR && ccst_cursor_count > 0)
		CCST_Score_TryDiscover(ccst_cursor);
}

static void CCST_Inv_SlotNormalize(int i) {
	if (i < 0 || i >= CCST_INV_SLOTS) return;
	if (ccst_slots[i] == BLOCK_AIR || Blocks.Draw[ccst_slots[i]] == DRAW_GAS) {
		ccst_slots[i] = BLOCK_AIR;
		ccst_slot_count[i] = 0;
		return;
	}
	if (ccst_slot_count[i] < 1) ccst_slot_count[i] = 1;
	if (ccst_slot_count[i] > CCST_INV_STACK_MAX) ccst_slot_count[i] = CCST_INV_STACK_MAX;
}

static void CCST_Inv_TransmuteCursorNormalize(void) {
	if (ccst_transmute == BLOCK_AIR || Blocks.Draw[ccst_transmute] == DRAW_GAS) {
		ccst_transmute = BLOCK_AIR;
		ccst_transmute_count = 0;
	} else {
		if (ccst_transmute_count < 1) ccst_transmute_count = 1;
		if (ccst_transmute_count > CCST_INV_STACK_MAX) ccst_transmute_count = CCST_INV_STACK_MAX;
	}
	if (ccst_cursor == BLOCK_AIR || Blocks.Draw[ccst_cursor] == DRAW_GAS) {
		ccst_cursor = BLOCK_AIR;
		ccst_cursor_count = 0;
	} else {
		if (ccst_cursor_count < 1) ccst_cursor_count = 1;
		if (ccst_cursor_count > CCST_INV_STACK_MAX) ccst_cursor_count = CCST_INV_STACK_MAX;
	}
}

static void CCST_Inv_SyncSlotToHotbar(int slot) {
	if (slot < 0 || slot >= 9) return;
	Inventory_Set(slot, ccst_slots[slot]);
}

static BlockID CCST_Inv_PickupBlockNormalize(BlockID b);
static void CCST_Inv_SyncToHotbar(void);

/* When vanilla hotbar IDs change (e.g. SetHotbar), align plugin rows 0..8 and reset stacks. */
static void CCST_Inv_SyncHotbarSlotsIntoStorage(void) {
	int i;

	/* In survival, the plugin hotbar is authoritative. Only the server is allowed to change it
	 * (via CPE SetHotbar), which calls CCST_Inv_OnServerSetHotbar. */
	if (CCST_Policy_PluginEnabled() && CCST_Policy_SurvivalInventoryEnabled()) return;

	for (i = 0; i < 9; i++) {
		BlockID ng = Inventory_Get(i);
		if (ng == ccst_slots[i]) continue;
		ccst_slots[i] = ng;
		ccst_slot_count[i] = (ng != BLOCK_AIR && Blocks.Draw[ng] != DRAW_GAS) ? 1 : 0;
	}
	/* Cheap idempotent scan: server may grant hotbar types without going through merge paths. */
	CCST_Inv_RefreshDiscovery();
}

void CCST_Inv_OnServerSetHotbar(int index, BlockID block) {
	if (index < 0 || index >= 9) return;
	if (!CCST_Policy_PluginEnabled()) return;
	/* Only meaningful while survival inventory rules are active. */
	if (!CCST_Health_IsSurvivalActive()) return;

	block = CCST_Inv_PickupBlockNormalize(block);
	ccst_slots[index]      = block;
	ccst_slot_count[index] = (block != BLOCK_AIR && Blocks.Draw[block] != DRAW_GAS) ? 1 : 0;
	CCST_Inv_SlotNormalize(index);
	CCST_Inv_RefreshDiscovery();
}

/* InputHandler (and not Game_UpdateBlock / protocol) raises UserEvents.BlockChanged for local place/delete. */
static void CCST_Inv_OnUserBlockChanged(void* obj, IVec3 coords, BlockID oldBlock, BlockID newBlock) {
	int slot;
	(void)obj;
	(void)coords;
	(void)oldBlock;

	if (!CCST_Policy_PluginEnabled() || !CCST_Policy_SurvivalInventoryEnabled()) return;
	if (CCST_Health_IsDeadOrDying()) return;

	if (newBlock == BLOCK_AIR || Blocks.Draw[newBlock] == DRAW_GAS) return;

	slot = Inventory.SelectedIndex;
	if (slot < 0 || slot >= 9) return;

	CCST_Inv_SyncHotbarSlotsIntoStorage();
	if (ccst_slot_count[slot] <= 0) return;
	if (!CCST_Inv_SameItemStack(ccst_slots[slot], newBlock)) return;

	ccst_slot_count[slot]--;
	if (ccst_slot_count[slot] <= 0) {
		ccst_slots[slot] = BLOCK_AIR;
		ccst_slot_count[slot] = 0;
	}
	CCST_Inv_SlotNormalize(slot);
	CCST_Inv_SyncSlotToHotbar(slot);
	Event_RaiseVoid(&UserEvents.HeldBlockChanged);
}

cc_bool CCST_Inv_TryConsumeOneSelected(BlockID expect) {
	int slot;

	if (!CCST_Policy_PluginEnabled() || !CCST_Policy_SurvivalInventoryEnabled()) return false;
	if (CCST_Health_IsDeadOrDying()) return false;

	slot = Inventory.SelectedIndex;
	if (slot < 0 || slot >= 9) return false;

	CCST_Inv_SyncHotbarSlotsIntoStorage();
	if (!CCST_Inv_SameItemStack(ccst_slots[slot], expect)) return false;
	if (ccst_slot_count[slot] <= 0) return false;

	ccst_slot_count[slot]--;
	if (ccst_slot_count[slot] <= 0) {
		ccst_slots[slot] = BLOCK_AIR;
		ccst_slot_count[slot] = 0;
	}
	CCST_Inv_SlotNormalize(slot);
	CCST_Inv_SyncSlotToHotbar(slot);
	Event_RaiseVoid(&UserEvents.HeldBlockChanged);
	return true;
}

static BlockID CCST_Inv_PickupBlockNormalize(BlockID b) {
	cc_bool mode = Options_GetBool(OPT_CLASSIC_MODE, false);
	cc_bool hacks = Options_GetBool(OPT_CLASSIC_HACKS, false);
	if (mode && !hacks) {
		if (b == BLOCK_GRASS) return BLOCK_DIRT;
		if (b == BLOCK_DOUBLE_SLAB) return BLOCK_SLAB;
	}
	if (b >= BLOCK_COUNT) return BLOCK_AIR;
	if (Blocks.Draw[b] == DRAW_GAS) return BLOCK_AIR;
	return b;
}

/* One merge step or one new-stack chunk; returns true if any items moved. */
static cc_bool CCST_Inv_TryMergeStashIntoGrid(BlockID* blk, int* cnt) {
	int i, space, mv;

	if (*blk == BLOCK_AIR || *cnt <= 0) return false;

	for (i = 0; i < CCST_INV_SLOTS; i++) {
		CCST_Inv_SlotNormalize(i);
		if (!CCST_Inv_SameItemStack(ccst_slots[i], *blk)) continue;
		if (ccst_slot_count[i] <= 0) continue;
		if (ccst_slot_count[i] >= CCST_INV_STACK_MAX) continue;

		space = CCST_INV_STACK_MAX - ccst_slot_count[i];
		mv = *cnt < space ? *cnt : space;
		ccst_slot_count[i] += mv;
		*cnt -= mv;
		CCST_Inv_SyncSlotToHotbar(i);
		if (*cnt <= 0) {
			*blk = BLOCK_AIR;
			*cnt = 0;
		}
		return true;
	}

	for (i = 0; i < CCST_INV_SLOTS; i++) {
		CCST_Inv_SlotNormalize(i);
		if (ccst_slots[i] != BLOCK_AIR && ccst_slot_count[i] > 0) continue;

		mv = *cnt < CCST_INV_STACK_MAX ? *cnt : CCST_INV_STACK_MAX;
		ccst_slots[i] = *blk;
		ccst_slot_count[i] = mv;
		*cnt -= mv;
		CCST_Inv_SyncSlotToHotbar(i);
		if (*cnt <= 0) {
			*blk = BLOCK_AIR;
			*cnt = 0;
		}
		return true;
	}
	return false;
}

cc_bool CCST_Inv_TryPickupMinedBlock(BlockID block) {
	BlockID b;
	int cnt;

	if (!CCST_Policy_PluginEnabled() || !CCST_Policy_SurvivalInventoryEnabled()) return false;

	b = CCST_Inv_PickupBlockNormalize(block);
	if (b == BLOCK_AIR) return false;

	CCST_Inv_SyncHotbarSlotsIntoStorage();
	cnt = 1;
	while (cnt > 0) {
		if (!CCST_Inv_TryMergeStashIntoGrid(&b, &cnt))
			return false;
	}
	CCST_Inv_RefreshDiscovery();
	Event_RaiseVoid(&UserEvents.HeldBlockChanged);
	return true;
}

void CCST_Inv_DrawHotbarStackCounts2D(void) {
	float hb, scaleX, scaleY;
	int i, c;
	int slotL, slotT, slotW, slotH;

	if (!CCST_Policy_PluginEnabled() || !CCST_Health_IsSurvivalActive()) return;
	if (CCST_Inv_IsOpen()) return;
	if (Gfx.LostContext) return;

	/* Prevent vanilla pick-block / other local changes from sticking in survival. */
	if (CCST_Policy_PluginEnabled() && CCST_Policy_SurvivalInventoryEnabled())
		CCST_Inv_SyncToHotbar();
	else
		CCST_Inv_SyncHotbarSlotsIntoStorage();

	/* Match Gui_GetHotbarScale (see CCST_Inv_GetHotbarScaleLinear). */
	hb = CCST_UIHotbarScaleLinear();
	scaleX = hb * DisplayInfo.ScaleX;
	scaleY = hb * DisplayInfo.ScaleY;

	/*
	 * Beta 1.7.3 GuiIngame: var16 = width/2 - 90 + slot*20 + 2; var17 = height - 16 - 3;
	 * (var16, var17) is the 16×16 item quad top-left passed to RenderItem (same as ClassiCube HUD hotbar).
	 */
	slotW = (int)(16.0f * scaleX + 0.5f);
	slotH = (int)(16.0f * scaleY + 0.5f);
	if (slotW < 1) slotW = 1;
	if (slotH < 1) slotH = 1;

	for (i = 0; i < 9; i++) {
		c = (int)ccst_slot_count[i];
		if (c <= 1) continue;
		/* Integer half-width like beta: width/2 - 90 + i*20 + 2 */
		slotL = Game.Width / 2 + (int)((-90.0f + 20.0f * (float)i + 2.0f) * scaleX + 0.5f);
		slotT = Game.Height + (int)(-19.0f * scaleY + 0.5f);
		CCST_Inv_DrawStackCountInItemSlot(c, slotL, slotT, slotW, slotH);
	}
}

/*
 * Build the list of transmutation targets for the current transmute-slot material.
 * AutoRotate-group variants (log rotations, slab orientations, etc.) share identical
 * textures/stuff scores so showing every rotation clutters the list, we keep only one
 * representative per family (canonical = the family's lowest BlockID), matching the
 * stack-merge behaviour of CCST_Inv_SameItemStack.
 *
 * Rows that would produce zero output at the current stack size are still included but
 * drawn dimmed (see CCST_Inv_Draw2D); they become viable again when the player adds
 * more material, so hiding them would be confusing.
 */
static void CCST_Inv_RebuildMutations(void) {
	BlockID from, from_canon;
	cc_bool seen[BLOCK_COUNT];
	int i;

	from = ccst_transmute;
	ccst_mut_count = 0;
	if (from == BLOCK_AIR || Blocks.Draw[from] == DRAW_GAS) return;
	if (CCST_BlockType_IsTransmuteExcluded(from)) return;

	from_canon = CCST_BlockFamily_Canonical(from);

	memset(seen, 0, sizeof(seen));
	seen[from_canon] = true;

	for (i = 1; i < BLOCK_COUNT; i++) {
		BlockID canon = CCST_BlockFamily_Canonical((BlockID)i);
		if (seen[canon]) continue;
		if (Blocks.Draw[canon] == DRAW_GAS) continue;
		if (CCST_BlockType_IsTransmuteExcluded(canon)) continue;
		if (!CCST_BlockType_CanTransmute(from, canon)) continue;
		seen[canon] = true;
		ccst_mut_blocks[ccst_mut_count++] = canon;
	}

	if (ccst_list_scroll > 0 && ccst_list_scroll >= ccst_mut_count)
		ccst_list_scroll = ccst_mut_count > 0 ? ccst_mut_count - 1 : 0;
}

static cc_bool CCST_Inv_ListHasScrollbar(void) {
	int vis = CCST_Inv_ListVisibleRows();
	return ccst_mut_count > vis;
}

/* Scrollbar visuals copied from Widgets.c ScrollbarWidget (not CC_API). */
#define CCST_SCROLL_BACK_COL  PackedCol_Make( 10,  10,  10, 220)
#define CCST_SCROLL_BAR_COL   PackedCol_Make(100, 100, 100, 220)
#define CCST_SCROLL_HOVER_COL PackedCol_Make(122, 122, 122, 220)

static void CCST_Inv_ScrollbarClampTopRow(struct CCST_ListScrollbar* w) {
	int maxTop = w->rowsTotal - w->rowsVisible;
	if (w->topRow >= maxTop) w->topRow = maxTop;
	if (w->topRow < 0) w->topRow = 0;
}

static float CCST_Inv_ScrollbarGetScale(struct CCST_ListScrollbar* w) {
	float rows = (float)w->rowsTotal;
	return (w->height - w->borderY * 2) / rows;
}

static void CCST_Inv_ScrollbarGetThumb(struct CCST_ListScrollbar* w, int* outY, int* outH) {
	float scale = CCST_Inv_ScrollbarGetScale(w);
	int y = (int)ceilf((float)w->topRow * scale) + w->borderY;
	int h = (int)ceilf((float)w->rowsVisible * scale);
	h = min(y + h, w->height - w->borderY) - y;
	*outY = y;
	*outH = h;
}

static void CCST_Inv_ScrollbarEnsureInited(struct CCST_ListScrollbar* w, int pxWidth) {
	if (ccst_inv_listScrollW == pxWidth) return;
	memset(w, 0, sizeof(*w));
	w->width     = pxWidth;
	w->borderX   = Display_ScaleX(2);
	w->borderY   = Display_ScaleY(2);
	w->nubsWidth = Display_ScaleX(3);
	w->offsets[0] = Display_ScaleY(-1 - 4);
	w->offsets[1] = Display_ScaleY(-1);
	w->offsets[2] = Display_ScaleY(-1 + 4);
	/* Touch padding (match Widgets.c behaviour) */
	w->padding = Gui_TouchUI ? Display_ScaleX(15) : 0;
	w->draggingId = -1;
	ccst_inv_listScrollW = pxWidth;
}

static void CCST_Inv_ListSyncScrollbarLayout(void) {
	int vis, maxScroll;
	int listX0, listY0, listH;
	int sbW;

	vis = CCST_Inv_ListVisibleRows();
	maxScroll = ccst_mut_count - vis;
	if (maxScroll < 0) maxScroll = 0;
	if (ccst_list_scroll < 0) ccst_list_scroll = 0;
	if (ccst_list_scroll > maxScroll) ccst_list_scroll = maxScroll;

	/* Match block picker scrollbar width (InventoryScreen uses 22 * OPT_INV_SCROLLBAR_SCALE). */
	sbW = (int)(22.0f * Options_GetFloat(OPT_INV_SCROLLBAR_SCALE, 0, 10, 1) + 0.5f);
	sbW = Display_ScaleX(sbW);
	if (sbW < 8) sbW = 8;

	CCST_Inv_ScrollbarEnsureInited(&ccst_inv_listScroll, sbW);

	listX0 = CCST_Inv_ScrX(MC_LIST_MCX);
	listY0 = CCST_Inv_ScrY(MC_LIST_TOP);
	listH  = CCST_Inv_ScrY(MC_LIST_BOTTOM) - listY0;
	if (listH < 1) listH = 1;

	/* Place scrollbar on the right edge of the list panel. */
	ccst_inv_listScroll.x      = listX0 + (CCST_Inv_ScrX(MC_LIST_MCX + MC_LIST_W) - listX0) - sbW;
	ccst_inv_listScroll.y      = listY0;
	ccst_inv_listScroll.width  = sbW;
	ccst_inv_listScroll.height = listH;
	ccst_inv_listScroll.rowsTotal   = ccst_mut_count;
	ccst_inv_listScroll.rowsVisible = vis;
	ccst_inv_listScroll.topRow      = ccst_list_scroll;
}

static void CCST_Inv_ListApplyScrollbarTopRow(void) {
	int vis, maxScroll;
	vis = CCST_Inv_ListVisibleRows();
	maxScroll = ccst_mut_count - vis;
	if (maxScroll < 0) maxScroll = 0;
	ccst_list_scroll = ccst_inv_listScroll.topRow;
	if (ccst_list_scroll < 0) ccst_list_scroll = 0;
	if (ccst_list_scroll > maxScroll) ccst_list_scroll = maxScroll;
	ccst_inv_listScroll.topRow = ccst_list_scroll;
}

static cc_bool CCST_Inv_ScrollbarPointerDown(struct CCST_ListScrollbar* w, int id, int x, int y) {
	int posY, h;

	if (w->draggingId >= 0 && w->draggingId == id) return true;
	if (x < w->x || x >= w->x + w->width + w->padding) return false;
	/* Only intercept pointer that's dragging scrollbar */
	if (w->draggingId >= 0) return false;

	y -= w->y;
	CCST_Inv_ScrollbarGetThumb(w, &posY, &h);
	if (y < posY) w->topRow -= w->rowsVisible;
	else if (y >= posY + h) w->topRow += w->rowsVisible;
	else {
		w->draggingId = id;
		w->dragOffset = y - posY;
	}
	CCST_Inv_ScrollbarClampTopRow(w);
	return true;
}

static void CCST_Inv_ScrollbarPointerUp(struct CCST_ListScrollbar* w, int id) {
	if (w->draggingId != id) return;
	w->draggingId = -1;
	w->dragOffset = 0;
}

static cc_bool CCST_Inv_ScrollbarPointerMove(struct CCST_ListScrollbar* w, int id, int y) {
	float scale;
	if (w->draggingId != id) return false;
	y -= w->y;
	scale = CCST_Inv_ScrollbarGetScale(w);
	if (scale <= 0.0f) return true;
	w->topRow = (int)((float)(y - w->dragOffset) / scale);
	CCST_Inv_ScrollbarClampTopRow(w);
	return true;
}

static void CCST_Inv_ScrollbarMouseScroll(struct CCST_ListScrollbar* w, float delta) {
	/* Simple wheel behaviour (enough to match picker feel). */
	if (delta > 0.0f) w->topRow--;
	else if (delta < 0.0f) w->topRow++;
	CCST_Inv_ScrollbarClampTopRow(w);
}

static void CCST_Inv_ScrollbarRender(struct CCST_ListScrollbar* w) {
	int x, y, w0, h;
	PackedCol barCol;
	cc_bool hovered;

	if (w->height <= 0 || w->width <= 0) return;
	x = w->x;
	w0 = w->width;
	CCST_Draw2D_Flat(&ccst_flatVb, x, w->y, w0, w->height, CCST_SCROLL_BACK_COL);

	CCST_Inv_ScrollbarGetThumb(w, &y, &h);
	x += w->borderX; y += w->y;
	w0 -= w->borderX * 2;
	if (w0 < 1) w0 = 1;

	/* Gui_ContainsPointers isn't exported for plugins in some builds; inline equivalent. */
	{
		int i;
		hovered = false;
		for (i = 0; i < Pointers_Count; i++) {
			int px = Pointers[i].x, py = Pointers[i].y;
			if (px >= x && py >= y && px < (x + w0) && py < (y + h)) { hovered = true; break; }
		}
	}
	barCol  = hovered ? CCST_SCROLL_HOVER_COL : CCST_SCROLL_BAR_COL;
	CCST_Draw2D_Flat(&ccst_flatVb, x, y, w0, h, barCol);

	if (h < 20) return;
	x += w->nubsWidth; y += (h / 2);
	w0 -= w->nubsWidth * 2;
	if (w0 < 1) w0 = 1;
	CCST_Draw2D_Flat(&ccst_flatVb, x, y + w->offsets[0], w0, w->borderY, CCST_SCROLL_BACK_COL);
	CCST_Draw2D_Flat(&ccst_flatVb, x, y + w->offsets[1], w0, w->borderY, CCST_SCROLL_BACK_COL);
	CCST_Draw2D_Flat(&ccst_flatVb, x, y + w->offsets[2], w0, w->borderY, CCST_SCROLL_BACK_COL);
}

static cc_bool CCST_Inv_ScrollbarContainsPoint(struct CCST_ListScrollbar* w, int mx, int my) {
	if (!w) return false;
	return mx >= w->x && my >= w->y && mx < (w->x + w->width + w->padding) && my < (w->y + w->height);
}

static void CCST_Inv_ListGetContentRectPx(int* outX, int* outY, int* outW, int* outH) {
	int x0, y0, x1, y1;
	int w, h;

	x0 = CCST_Inv_ScrX(MC_LIST_MCX);
	y0 = CCST_Inv_ScrY(MC_LIST_TOP);
	x1 = CCST_Inv_ScrX(MC_LIST_MCX + MC_LIST_W);
	y1 = CCST_Inv_ScrY(MC_LIST_BOTTOM);
	w = x1 - x0;
	h = y1 - y0;
	if (w < 1) w = 1;
	if (h < 1) h = 1;

	/* If scrollbar exists, reserve its area (plus border) so highlights/overlays don't cover it. */
	if (CCST_Inv_ListHasScrollbar()) {
		CCST_Inv_ListSyncScrollbarLayout();
		w = ccst_inv_listScroll.x - x0;
		if (w < 1) w = 1;
	}

	if (outX) *outX = x0;
	if (outY) *outY = y0;
	if (outW) *outW = w;
	if (outH) *outH = h;
}

/*
 * Room available on the cursor for another `to`-stack: either the cursor is empty (full
 * stack), or it already holds `to` / a rotation variant (remaining space). Different-type
 * cursor returns 0, caller must reject.
 */
static int CCST_Inv_CursorRoomFor(BlockID to) {
	if (ccst_cursor == BLOCK_AIR || ccst_cursor_count <= 0)
		return CCST_INV_STACK_MAX;
	if (!CCST_Inv_SameItemStack(ccst_cursor, to))
		return 0;
	return CCST_INV_STACK_MAX - ccst_cursor_count;
}

/*
 * Crafting-result semantics (MCP-919 GuiContainer):
 *   - Click a recipe row -> consume `consumed` of `from` from the transmute slot and
 *     deposit `produced` of `to` on the cursor (merging if cursor already holds `to`).
 *   - Leftover input stays in the transmute slot; it's not pushed into the grid so the
 *     player can see what's still consumable.
 *   - Reject cleanly when the cursor is busy with a different type, so the player doesn't
 *     lose the held item.
 */
static void CCST_Inv_ApplyTransmuteTowards(BlockID to) {
	int produced, consumed, room;
	cc_bool prob_partial;
	BlockID from;
	cc_string msg;
	char buf[STRING_SIZE];

	if (ccst_transmute == BLOCK_AIR || ccst_transmute_count <= 0) {
		CCST_Chat("&cPut blocks in the transmute slot first.");
		return;
	}
	from = ccst_transmute;
	if (CCST_Inv_SameItemStack(from, to)) {
		CCST_Chat("&7Select a different block type in the list.");
		return;
	}
	if (!CCST_BlockType_CanTransmute(from, to)) {
		CCST_Chat("&cThose materials cannot be exchanged.");
		return;
	}

	room = CCST_Inv_CursorRoomFor(to);
	if (room <= 0) {
		CCST_Chat("&cCursor is busy, drop the held stack first.");
		return;
	}

	prob_partial = false;
	CCST_BlockType_TransmuteExchange(from, ccst_transmute_count, to,
		room, &produced, &consumed, &prob_partial);
	if (produced <= 0 || consumed <= 0) {
		CCST_Chat("&cNot enough material for even one of that block, add more to the transmute slot.");
		return;
	}

	if (ccst_cursor == BLOCK_AIR || ccst_cursor_count <= 0) {
		ccst_cursor = to;
		ccst_cursor_count = produced;
	} else {
		ccst_cursor_count += produced;
	}

	ccst_transmute_count -= consumed;
	if (ccst_transmute_count <= 0) {
		ccst_transmute = BLOCK_AIR;
		ccst_transmute_count = 0;
	}
	CCST_Inv_TransmuteCursorNormalize();

	CCST_Inv_RebuildMutations();
	CCST_Inv_RefreshDiscovery();
	Event_RaiseVoid(&UserEvents.HeldBlockChanged);

	String_InitArray(msg, buf);
	String_AppendConst(&msg, "&aExchanged &f");
	String_AppendInt(&msg, consumed);
	String_AppendConst(&msg, "&7 -> &f");
	String_AppendInt(&msg, produced);
	String_AppendConst(&msg, "&7.");
	if (prob_partial)
		String_AppendConst(&msg, " &7(remainder: chance-based yield)");
	Chat_Add(&msg);
}

static void CCST_Inv_SyncFromHotbar(void) {
	CCST_Inv_SyncHotbarSlotsIntoStorage();
}

static void CCST_Inv_SyncToHotbar(void) {
	int i;
	for (i = 0; i < 9; i++)
		Inventory_Set(i, ccst_slots[i]);
}

static void CCST_VanillaInventory_Hide(void) {
	struct Screen* s = Gui_GetScreen(GUI_PRIORITY_INVENTORY);
	if (s) Gui_Remove(s);
}

static void CCST_Inv_OpenInternal(void) {
	CCST_VanillaInventory_Hide();
	CCST_Inv_SyncFromHotbar();
	CCST_InvGrab_InitOnce();
	Gui_Add(&ccst_invGrabScreen, GUI_PRIORITY_INVENTORY);
	ccst_inv_open = true;
	ccst_cursor = BLOCK_AIR;
	ccst_cursor_count = 0;
	ccst_list_scroll = 0;
	/* Reset scrollbar drag state (in case a pointer-up was missed). */
	ccst_inv_listScroll.draggingId = -1;
	ccst_inv_listScroll.dragOffset = 0;
	CCST_Inv_RebuildMutations();
	CCST_Persist_MarkDirty();
}

static void CCST_Inv_CloseInternal(void) {
	int j;

	Gui_Remove(&ccst_invGrabScreen);
	/* Ensure we never keep dragging across closes. */
	ccst_inv_listScroll.draggingId = -1;
	ccst_inv_listScroll.dragOffset = 0;
	for (j = 0; j < CCST_INV_SLOTS; j++)
		CCST_Inv_SlotNormalize(j);
	CCST_Inv_TransmuteCursorNormalize();

	for (;;) {
		cc_bool progressed = false;
		if (ccst_cursor_count <= 0 && ccst_transmute_count <= 0) break;

		if (ccst_cursor_count > 0 && CCST_Inv_TryMergeStashIntoGrid(&ccst_cursor, &ccst_cursor_count))
			progressed = true;
		else if (ccst_transmute_count > 0 && CCST_Inv_TryMergeStashIntoGrid(&ccst_transmute, &ccst_transmute_count))
			progressed = true;

		if (!progressed) break;
	}
	if (ccst_cursor_count > 0 || ccst_transmute_count > 0)
		CCST_Chat("&cInventory full, free a slot (cursor/transmute items were not merged).");
	CCST_Inv_TransmuteCursorNormalize();
	CCST_Inv_RefreshDiscovery();
	CCST_Inv_SyncToHotbar();
	ccst_inv_open = false;
	CCST_VanillaInventory_Hide();
}

/* Close overlay when MOTD policy removes inventory (e.g. -inv) without a map reload. */
static void CCST_Inv_CloseIfPolicyDisallows(void) {
	if (!ccst_inv_open) return;
	if (!CCST_Policy_PluginEnabled() || !CCST_Policy_SurvivalInventoryEnabled())
		CCST_Inv_CloseInternal();
}

cc_bool CCST_Inv_IsOpen(void) {
	return ccst_inv_open;
}

void CCST_Inv_GetState(cc_uint16 blocks[CCST_INV_SLOTS], cc_uint8 counts[CCST_INV_SLOTS],
	cc_uint16* transmuteBlock, cc_uint8* transmuteCount) {
	int i;
	if (blocks) {
		for (i = 0; i < CCST_INV_SLOTS; i++)
			blocks[i] = (cc_uint16)ccst_slots[i];
	}
	if (counts) {
		for (i = 0; i < CCST_INV_SLOTS; i++) {
			int c = ccst_slot_count[i];
			if (c < 0) c = 0;
			if (c > 255) c = 255;
			counts[i] = (cc_uint8)c;
		}
	}
	if (transmuteBlock) *transmuteBlock = (cc_uint16)ccst_transmute;
	if (transmuteCount) {
		int c = ccst_transmute_count;
		if (c < 0) c = 0;
		if (c > 255) c = 255;
		*transmuteCount = (cc_uint8)c;
	}
}

void CCST_Inv_SetState(const cc_uint16 blocks[CCST_INV_SLOTS], const cc_uint8 counts[CCST_INV_SLOTS],
	BlockID transmuteBlock, int transmuteCount) {
	int i;
	if (!blocks || !counts) return;

	/* Never restore into an open overlay (avoid weird cursor/transmute merge semantics). */
	if (ccst_inv_open) CCST_Inv_CloseInternal();

	for (i = 0; i < CCST_INV_SLOTS; i++) {
		ccst_slots[i] = (BlockID)(blocks[i] % BLOCK_COUNT);
		ccst_slot_count[i] = (int)counts[i];
		CCST_Inv_SlotNormalize(i);
	}
	ccst_transmute = (BlockID)(transmuteBlock % BLOCK_COUNT);
	ccst_transmute_count = transmuteCount;
	CCST_Inv_TransmuteCursorNormalize();
	CCST_Inv_RefreshDiscovery();

	/* Apply to vanilla hotbar immediately. */
	CCST_Inv_SyncToHotbar();
	Event_RaiseVoid(&UserEvents.HeldBlockChanged);
}

void CCST_Inv_SetOpen(cc_bool open) {
	if (open) CCST_Inv_OpenInternal();
	else CCST_Inv_CloseInternal();
}

void CCST_Inv_ClearAll(void) {
	int i;

	if (CCST_Inv_IsOpen())
		CCST_Inv_SetOpen(false);

	for (i = 0; i < INVENTORY_HOTBARS * INVENTORY_BLOCKS_PER_HOTBAR; i++)
		Inventory.Table[i] = BLOCK_AIR;

	Inventory.Offset = 0;
	Inventory.SelectedIndex = 0;

	for (i = 0; i < CCST_INV_SLOTS; i++) {
		ccst_slots[i] = BLOCK_AIR;
		ccst_slot_count[i] = 0;
	}
	ccst_transmute = BLOCK_AIR;
	ccst_transmute_count = 0;
	ccst_cursor = BLOCK_AIR;
	ccst_cursor_count = 0;
	ccst_list_scroll = 0;
	CCST_Inv_RebuildMutations();

	Event_RaiseVoid(&UserEvents.HeldBlockChanged);
}

void CCST_Inv_OnMapBegun(void) {
	if (!CCST_Policy_PluginEnabled() || !CCST_Policy_SurvivalInventoryEnabled()) return;
	CCST_Inv_ClearAll();
}

static void CCST_Inv_ComputeLayout(int* ox, int* oy) {
	float invLin, root;
	int cellSize, blockB, blockPx, sx, sy, pw, ph;

	/*
	 * TableWidget_Reposition (Widgets.c): w->scale = Gui_GetInventoryScale(), scale = sqrt(w->scale),
	 * cellSizeX = Display_ScaleX(cellSize * scale). That cell width is ONE block slot in the picker.
	 *
	 * Beta inventory texture uses MC_STEP (18) between slot origins, one slot pitch, not 18/48 of
	 * a ClassiCube cell. So stride (px per MC unit in ScrX/Y) must make Δmc=MC_STEP equal one cell:
	 *   ScrX(mc+MC_STEP)-ScrX(mc) = cellSizeX  =>  strideX = cellSizeX (see ScrX: mc * stride / MC_STEP).
	 * The old (cellW * MC_STEP / cellSize) made each slot ~18/48 of the picker cell (~2.67× too small).
	 */
	invLin = CCST_UIInventoryScaleLinear();
	root = Math_SqrtF(invLin);
	cellSize = Gui.ClassicInventory ? 48 : 50;
	blockB = Gui.ClassicInventory ? 40 : 50;

	sx = Display_ScaleX(cellSize * root);
	sy = Display_ScaleY(cellSize * root);
	if (sx < 14) sx = 14;
	if (sy < 14) sy = 14;

	blockPx = Display_ScaleX(blockB * root);
	ccst_layout_iconHalf = ((float)blockPx * 0.7f) / 2.0f;
	if (ccst_layout_iconHalf < 10.0f) ccst_layout_iconHalf = 10.0f;

	pw = (MC_FULL_W * sx) / MC_STEP;
	ph = (MC_PANEL_H * sy) / MC_STEP;

	*ox = (Game.Width  - pw) / 2;
	*oy = (Game.Height - ph) / 2;
	ccst_layout_ox = *ox;
	ccst_layout_oy = *oy;
	ccst_layout_strideX = sx;
	ccst_layout_strideY = sy;
}

static int CCST_Inv_ScrX(int mcX) {
	return ccst_layout_ox + (mcX * ccst_layout_strideX) / MC_STEP;
}

static int CCST_Inv_ScrY(int mcY) {
	return ccst_layout_oy + (mcY * ccst_layout_strideY) / MC_STEP;
}

/*
 * Inverse of ScrX/ScrY: integer division in the forward map makes row/column heights uneven at
 * large strides; (delta*MC_STEP+stride/2)/stride can be off by one vs the hotbar band (mcy 142..).
 * Use largest m with Scr(m) <= screen (monotone) so hit-testing matches drawn geometry.
 */
static int CCST_Inv_McX(int screenX) {
	int lo, hi, mid;

	if (screenX < ccst_layout_ox) return -1;
	lo = 0;
	hi = MC_FULL_W;
	while (lo < hi) {
		mid = (lo + hi + 1) >> 1;
		if (CCST_Inv_ScrX(mid) <= screenX) lo = mid; else hi = mid - 1;
	}
	return lo;
}

static int CCST_Inv_McY(int screenY) {
	int lo, hi, mid;

	if (screenY < ccst_layout_oy) return -1;
	lo = 0;
	hi = MC_PANEL_H;
	while (lo < hi) {
		mid = (lo + hi + 1) >> 1;
		if (CCST_Inv_ScrY(mid) <= screenY) lo = mid; else hi = mid - 1;
	}
	return lo;
}

static cc_bool CCST_Inv_HitMcRect(int mx, int my, int mcx, int mcy, int mcw, int mch) {
	int x0 = CCST_Inv_ScrX(mcx);
	int y0 = CCST_Inv_ScrY(mcy);
	int x1 = CCST_Inv_ScrX(mcx + mcw);
	int y1 = CCST_Inv_ScrY(mcy + mch);
	if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
	if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
	return mx >= x0 && mx < x1 && my >= y0 && my < y1;
}

/* Returns slot 0..35, CCST_SLOT_TRANSMUTE, -2 list (use hover_list_row), -1 none. */
static int CCST_Inv_HitSlot(int mx, int my) {
	int mcx, mcy, col, row;

	mcx = CCST_Inv_McX(mx);
	mcy = CCST_Inv_McY(my);

	if (mcx < 0 || mcy < 0 || mcx >= MC_FULL_W || mcy >= MC_PANEL_H) return -1;

	/* Transmute slot: former 2×2 top-left cell (ContainerPlayer crafting). */
	if (mcx >= MC_TRANSMUTE_X && mcx < MC_TRANSMUTE_X + MC_SLOT && mcy >= MC_TRANSMUTE_Y && mcy < MC_TRANSMUTE_Y + MC_SLOT)
		return CCST_SLOT_TRANSMUTE;

	/* Main 3×9 */
	if (mcy >= MC_GRID_Y && mcy < MC_GRID_Y + 3 * MC_STEP) {
		col = (mcx - 8) / MC_STEP;
		row = (mcy - MC_GRID_Y) / MC_STEP;
		if (col >= 0 && col < 9 && row >= 0 && row < 3)
			return 9 + col + row * 9;
	}

	/* Hotbar */
	if (mcy >= MC_HOTBAR_Y && mcy < MC_HOTBAR_Y + MC_STEP) {
		col = (mcx - 8) / MC_STEP;
		if (col >= 0 && col < 9 && mcx >= 8 && mcx < 8 + 9 * MC_STEP)
			return col;
	}

	/* Mutation list (only MC_PANEL_W region is "inventory"; list is to the right). */
	if (mcx >= MC_LIST_MCX && mcx < MC_LIST_MCX + MC_LIST_W && mcy >= MC_LIST_TOP && mcy < MC_LIST_BOTTOM)
		return -2;

	return -1;
}

static int CCST_Inv_ListVisibleRows(void) {
	int listHMc = MC_LIST_BOTTOM - MC_LIST_TOP;
	return listHMc / MC_STEP;
}

static cc_bool CCST_Inv_ListHitRow(int mx, int my, int* outRow) {
	int mcx, mcy, rel, row, vis;

	/* If a scrollbar is present and the pointer is over it, don't treat it as a list row. */
	if (CCST_Inv_ListHasScrollbar()) {
		CCST_Inv_ListSyncScrollbarLayout();
		if (CCST_Inv_ScrollbarContainsPoint(&ccst_inv_listScroll, mx, my)) return false;
	}

	mcx = CCST_Inv_McX(mx);
	mcy = CCST_Inv_McY(my);
	if (mcx < MC_LIST_MCX || mcx >= MC_LIST_MCX + MC_LIST_W) return false;
	if (mcy < MC_LIST_TOP || mcy >= MC_LIST_BOTTOM) return false;

	rel = mcy - MC_LIST_TOP;
	row = rel / MC_STEP;
	vis = CCST_Inv_ListVisibleRows();
	if (row < 0 || row >= vis) return false;
	*outRow = row;
	return true;
}

static void CCST_Inv_UpdateHover(int mx, int my) {
	int s, lr;

	ccst_hover_kind = 0;
	ccst_hover_slot = -1;
	ccst_hover_list_row = -1;

	/* Don't show list hover highlight while hovering/dragging the scrollbar. */
	if (CCST_Inv_ListHasScrollbar()) {
		CCST_Inv_ListSyncScrollbarLayout();
		if (CCST_Inv_ScrollbarContainsPoint(&ccst_inv_listScroll, mx, my)) return;
	}

	if (CCST_Inv_ListHitRow(mx, my, &lr)) {
		int idx = ccst_list_scroll + lr;
		if (idx >= 0 && idx < ccst_mut_count) {
			ccst_hover_kind = 2;
			ccst_hover_list_row = lr;
		}
		return;
	}

	s = CCST_Inv_HitSlot(mx, my);
	if (s == -2) return;
	if (s >= 0) {
		ccst_hover_kind = 1;
		ccst_hover_slot = s;
	}
}

/*
 * Right-click slot semantics, mirroring Beta 1.7.3 GuiContainer.slotClick(mode=0, button=1):
 *   - Cursor empty, slot has stack  -> pick up ceil(slot/2) into cursor, leave floor in slot.
 *   - Cursor has stack, slot empty  -> drop exactly one into the slot.
 *   - Cursor has stack, slot same-type -> deposit one more (respecting the 64 cap).
 *   - Cursor has stack, slot different-type -> no-op (Beta actually swaps here, but for a
 *     survival layer with no trash bin that's a footgun; the user can left-click to swap).
 * The transmute slot uses the same machinery by aliasing ccst_transmute / ccst_transmute_count.
 */
static void CCST_Inv_RightClickSlot(int slot) {
	BlockID* p;
	int* pc;
	int take, space;

	if (slot == CCST_SLOT_TRANSMUTE) {
		CCST_Inv_TransmuteCursorNormalize();
		p  = &ccst_transmute;
		pc = &ccst_transmute_count;
	} else if (slot >= 0 && slot < CCST_INV_SLOTS) {
		CCST_Inv_SlotNormalize(slot);
		p  = &ccst_slots[slot];
		pc = &ccst_slot_count[slot];
	} else {
		return;
	}

	if (ccst_cursor == BLOCK_AIR || ccst_cursor_count <= 0) {
		if (*p == BLOCK_AIR || *pc <= 0) return;
		/* ceil(half) so a stack of 1 picks that one, a stack of 3 picks 2. */
		take = (*pc + 1) / 2;
		ccst_cursor = *p;
		ccst_cursor_count = take;
		*pc -= take;
		if (*pc <= 0) {
			*p = BLOCK_AIR;
			*pc = 0;
		}
	} else {
		if (*p == BLOCK_AIR || *pc <= 0) {
			*p = ccst_cursor;
			*pc = 1;
			ccst_cursor_count--;
			if (ccst_cursor_count <= 0) {
				ccst_cursor = BLOCK_AIR;
				ccst_cursor_count = 0;
			}
		} else if (CCST_Inv_SameItemStack(*p, ccst_cursor)) {
			space = CCST_INV_STACK_MAX - *pc;
			if (space <= 0) return;
			*pc += 1;
			ccst_cursor_count--;
			if (ccst_cursor_count <= 0) {
				ccst_cursor = BLOCK_AIR;
				ccst_cursor_count = 0;
			}
		} else {
			return;
		}
	}

	if (slot >= 0 && slot < 9)
		CCST_Inv_SyncSlotToHotbar(slot);
	if (slot == CCST_SLOT_TRANSMUTE)
		CCST_Inv_RebuildMutations();
	CCST_Inv_RefreshDiscovery();
}

static void CCST_Inv_SwapWithSlot(int slot) {
	BlockID* p;
	int* pc;
	BlockID tb;
	int tc;
	int i;

	if (slot == CCST_SLOT_TRANSMUTE) {
		p  = &ccst_transmute;
		pc = &ccst_transmute_count;
	} else {
		p  = &ccst_slots[slot];
		pc = &ccst_slot_count[slot];
	}

	tb = ccst_cursor;
	tc = ccst_cursor_count;
	ccst_cursor = *p;
	ccst_cursor_count = *pc;
	*p = tb;
	*pc = tc;

	if (slot >= 0 && slot < CCST_INV_SLOTS)
		CCST_Inv_SlotNormalize(slot);
	CCST_Inv_TransmuteCursorNormalize();

	for (i = 0; i < 9; i++)
		CCST_Inv_SyncSlotToHotbar(i);

	CCST_Inv_RebuildMutations();
	CCST_Inv_RefreshDiscovery();
}

/* Merge cursor into slot when same stack type (incl. AutoRotate group); else caller swaps. */
static cc_bool CCST_Inv_TryMergeCursorIntoSlot(int slot) {
	BlockID* p;
	int* pc;
	int space, mv;

	if (ccst_cursor_count <= 0 || ccst_cursor == BLOCK_AIR) return false;

	if (slot == CCST_SLOT_TRANSMUTE) {
		CCST_Inv_TransmuteCursorNormalize();
		p  = &ccst_transmute;
		pc = &ccst_transmute_count;
	} else if (slot >= 0 && slot < CCST_INV_SLOTS) {
		CCST_Inv_SlotNormalize(slot);
		p  = &ccst_slots[slot];
		pc = &ccst_slot_count[slot];
	} else {
		return false;
	}

	if (*p == BLOCK_AIR || *pc <= 0) return false;
	if (!CCST_Inv_SameItemStack(*p, ccst_cursor)) return false;

	space = CCST_INV_STACK_MAX - *pc;
	if (space <= 0) return false;

	mv = ccst_cursor_count < space ? ccst_cursor_count : space;
	*pc += mv;
	ccst_cursor_count -= mv;
	if (ccst_cursor_count <= 0) {
		ccst_cursor = BLOCK_AIR;
		ccst_cursor_count = 0;
	}

	if (slot >= 0 && slot < 9)
		CCST_Inv_SyncSlotToHotbar(slot);

	CCST_Inv_RebuildMutations();
	CCST_Inv_RefreshDiscovery();
	return true;
}

static void CCST_Inv_ApplyMutationClick(int listRow) {
	int idx = ccst_list_scroll + listRow;
	BlockID pick;

	if (idx < 0 || idx >= ccst_mut_count) return;
	if (ccst_transmute == BLOCK_AIR) return;

	pick = ccst_mut_blocks[idx];
	CCST_Inv_ApplyTransmuteTowards(pick);
}

static void CCST_Inv_OnPointerDown(void* obj, int idx) {
	(void)obj;
	if (CCST_Health_IsDeadOrDying()) return;
	if (!ccst_inv_open) return;
	if (idx != 0) return;

	{
		int mx = Pointers[idx].x, my = Pointers[idx].y;
		int lr;

		/* Scrollbar gets priority over list-row hit. */
		if (CCST_Inv_ListHasScrollbar()) {
			CCST_Inv_ListSyncScrollbarLayout();
			if (CCST_Inv_ScrollbarPointerDown(&ccst_inv_listScroll, idx, mx, my)) {
				CCST_Inv_ListApplyScrollbarTopRow();
				return;
			}
		}

		if (CCST_Inv_ListHitRow(mx, my, &lr)) {
			CCST_Inv_ApplyMutationClick(lr);
			return;
		}

		{
			int s = CCST_Inv_HitSlot(mx, my);
			if (s >= 0 && !CCST_Inv_TryMergeCursorIntoSlot(s))
				CCST_Inv_SwapWithSlot(s);
		}
	}
}

static void CCST_Inv_OnPointerUp(void* obj, int idx) {
	(void)obj;
	if (CCST_Health_IsDeadOrDying()) return;
	if (!ccst_inv_open) return;
	if (idx != 0) return;
	/* Always release if we were dragging, even if the list shrank mid-drag. */
	if (ccst_inv_listScroll.draggingId == idx) {
		CCST_Inv_ScrollbarPointerUp(&ccst_inv_listScroll, idx);
		CCST_Inv_ListApplyScrollbarTopRow();
	}
}

static void CCST_Inv_OnPointerMoved(void* obj, int idx) {
	(void)obj;
	if (CCST_Health_IsDeadOrDying()) return;
	if (!ccst_inv_open) return;
	if (idx != 0) return;
	if (CCST_Inv_ListHasScrollbar()) {
		CCST_Inv_ListSyncScrollbarLayout();
		(void)CCST_Inv_ScrollbarPointerMove(&ccst_inv_listScroll, idx, Pointers[idx].y);
		CCST_Inv_ListApplyScrollbarTopRow();
	}
	CCST_Inv_UpdateHover(Pointers[idx].x, Pointers[idx].y);
}

static void CCST_Inv_OnWheel(void* obj, float delta) {
	int mx, my, vis, maxScroll;

	(void)obj;
	if (CCST_Health_IsDeadOrDying()) return;
	if (!ccst_inv_open) return;
	if (!CCST_Policy_PluginEnabled() || !CCST_Health_IsSurvivalActive()) return;

	mx = Pointers[0].x;
	my = Pointers[0].y;
	if (!CCST_Inv_HitMcRect(mx, my, MC_LIST_MCX, MC_LIST_TOP, MC_LIST_W, MC_LIST_BOTTOM - MC_LIST_TOP))
		return;

	vis = CCST_Inv_ListVisibleRows();
	maxScroll = ccst_mut_count - vis;
	if (maxScroll < 0) maxScroll = 0;

	/* Prefer native scrollbar wheel behaviour when it exists. */
	if (CCST_Inv_ListHasScrollbar()) {
		CCST_Inv_ListSyncScrollbarLayout();
		CCST_Inv_ScrollbarMouseScroll(&ccst_inv_listScroll, delta);
		CCST_Inv_ListApplyScrollbarTopRow();
		return;
	}

	if (delta > 0.0f)
		ccst_list_scroll--;
	else
		ccst_list_scroll++;

	if (ccst_list_scroll < 0) ccst_list_scroll = 0;
	if (ccst_list_scroll > maxScroll) ccst_list_scroll = maxScroll;
}

/*
 * Right-click dispatcher. PointerEvents.Down only fires for CCMOUSE_L (see Input.c:225) so
 * the split-stack / place-one behaviour has to be wired through InputEvents.Down2 with
 * key == CCMOUSE_R. A Down2 for the right button arrives before any game-world place
 * would run, and we consume it (return true) when the overlay is open so InputHandler
 * won't also trigger a world block-place.
 */
static void CCST_Inv_HandleRightClick(int mx, int my) {
	int lr, s;

	if (CCST_Inv_ListHitRow(mx, my, &lr)) {
		CCST_Inv_ApplyMutationClick(lr);
		return;
	}
	s = CCST_Inv_HitSlot(mx, my);
	if (s >= 0)
		CCST_Inv_RightClickSlot(s);
}

static void CCST_Inv_OnInputDown2(void* obj, int key, cc_bool repeating, struct InputDevice* device) {
	(void)obj;

	if (CCST_Health_IsDeadOrDying()) return;
	CCST_Inv_CloseIfPolicyDisallows();
	if (!CCST_Policy_PluginEnabled()) return;

	/*
	 * -inv policy:
	 * - If survival is forced: disable the inventory bind entirely (no survival inv, no creative picker).
	 * - Otherwise: let the engine's normal inventory/block-picker behavior handle it.
	 */
	if (!CCST_Policy_SurvivalInventoryEnabled()) {
		if (CCST_Health_IsSurvivalActive() && CCST_Policy_IsGamemodeForced()) {
			if (!repeating && CCST_InputBind_Claims(BIND_INVENTORY, key, device)) {
				/* Hide any screen that may have been opened by the engine this frame. */
				CCST_VanillaInventory_Hide();
			}
		} else if (!repeating && CCST_InputBind_Claims(BIND_INVENTORY, key, device)) {
			CCST_Persist_MarkDirty();
		}
		return;
	}

	/*
	 * Do not open the survival inventory while the user is typing in chat.
	 * (Other grab screens, like the vanilla inventory/picker that may open earlier in the frame,
	 * should not block the survival inventory - we just replace them.)
	 */
	if (!ccst_inv_open && Gui.InputGrab) {
		struct Screen* chat = Gui_GetScreen(GUI_PRIORITY_CHAT);
		if (chat && (struct Screen*)Gui.InputGrab == chat) {
			/* Still consume the bind so creative picker doesn't open underneath chat typing. */
			if (!repeating && CCST_InputBind_Claims(BIND_INVENTORY, key, device)) {
				CCST_VanillaInventory_Hide();
			}
			return;
		}
	}

	if (ccst_inv_open) {
		if (repeating) return;
		if (key == device->escapeButton) {
			CCST_Inv_CloseInternal();
			return;
		}
		if (key == CCMOUSE_R) {
			CCST_Inv_HandleRightClick(Pointers[0].x, Pointers[0].y);
			return;
		}
	}

	if (repeating) return;

	if (CCST_InputBind_Claims(BIND_INVENTORY, key, device)) {
		/* Replace any vanilla inventory/block-picker opened this frame. */
		CCST_VanillaInventory_Hide();
		if (ccst_inv_open)
			CCST_Inv_CloseInternal();
		else
			CCST_Inv_OpenInternal();
	}
}

/* Row-wide variant: same gradient as the slot highlight but spans a custom MC-unit rect,
   used for the mutation list where the hit region covers the whole text+icon strip. */
static void CCST_Inv_DrawRectHoverHighlight(int mcx, int mcy, int mcw, int mch, cc_bool hover) {
	int x, y, w, h;

	if (!hover) return;

	x = CCST_Inv_ScrX(mcx);
	y = CCST_Inv_ScrY(mcy);
	w = CCST_Inv_ScrX(mcx + mcw) - x;
	h = CCST_Inv_ScrY(mcy + mch) - y;

	CCST_Draw2D_Gradient(&ccst_flatVb, x, y, w, h, CCST_SEL_GRAD_TOP, CCST_SEL_GRAD_BOT);
}

/* TableWidget classic cell highlight: 10% inset, white vertical gradient (Widgets.c). */
static void CCST_Inv_DrawSlotHoverHighlight(int mcx, int mcy, cc_bool hover) {
	int x, y, w, h, gw, gh;
	float offX, offY;

	if (!hover) return;

	x = CCST_Inv_ScrX(mcx);
	y = CCST_Inv_ScrY(mcy);
	w = CCST_Inv_ScrX(mcx + MC_SLOT) - x;
	h = CCST_Inv_ScrY(mcy + MC_SLOT) - y;
	offX = (float)w * 0.1f;
	offY = (float)h * 0.1f;
	gw = (int)((float)w + 2.0f * offX + 0.5f);
	gh = (int)((float)h + 2.0f * offY + 0.5f);

	CCST_Draw2D_Gradient(&ccst_flatVb,
		(int)((float)x - offX + 0.5f), (int)((float)y - offY + 0.5f),
		gw, gh, CCST_SEL_GRAD_TOP, CCST_SEL_GRAD_BOT);
}

void CCST_Inv_Draw2D(float delta) {
	struct VertexTextured* verts;
	struct VertexTextured* v;
	struct _DrawerData ccst_drawer_saved;
	int count, ox, oy;
	int col, row, slot, cx, cy;
	float half;
	int i, vis, listMcX, sc;
	int sL, sT, sW, sH;
	float ch;
	(void)delta;

	CCST_Inv_CloseIfPolicyDisallows();
	if (!ccst_inv_open) return;

	CCST_Inv_ComputeLayout(&ox, &oy);
	CCST_Inv_UpdateHover(Pointers[0].x, Pointers[0].y);

	for (i = 0; i < CCST_INV_SLOTS; i++)
		CCST_Inv_SlotNormalize(i);
	CCST_Inv_TransmuteCursorNormalize();

	/* No full-screen dim, matches ClassiCube's creative block picker (world stays visible). */

	{
		int pw = (MC_FULL_W * ccst_layout_strideX) / MC_STEP;
		int ph = (MC_PANEL_H * ccst_layout_strideY) / MC_STEP;
		CCST_Draw2D_Gradient(&ccst_flatVb, ox, oy, pw, ph, CCST_TABLE_GRAD_TOP, CCST_TABLE_GRAD_BOT);
	}

	half = ccst_layout_iconHalf;

	/* Like the creative picker: show hovered block name + ID at top */
	CCST_Inv_DrawHoveredBlockTitle();

	/* Mutation list panel: same gradient as ClassiCube TableWidget (draw before per-cell hovers). */
	listMcX = MC_LIST_MCX;
	CCST_Draw2D_Gradient(&ccst_flatVb, CCST_Inv_ScrX(listMcX), CCST_Inv_ScrY(MC_LIST_TOP),
		CCST_Inv_ScrX(listMcX + MC_LIST_W) - CCST_Inv_ScrX(listMcX),
		CCST_Inv_ScrY(MC_LIST_BOTTOM) - CCST_Inv_ScrY(MC_LIST_TOP),
		CCST_TABLE_GRAD_TOP, CCST_TABLE_GRAD_BOT);

	/* Hover highlights on top (Gui.ClassicInventory cell style in TableWidget_Render2). */
	/* Always show a subtle outline around the transmute slot so it's easy to find. */
	{
		int x0 = CCST_Inv_ScrX(MC_TRANSMUTE_X);
		int y0 = CCST_Inv_ScrY(MC_TRANSMUTE_Y);
		int x1 = CCST_Inv_ScrX(MC_TRANSMUTE_X + MC_SLOT);
		int y1 = CCST_Inv_ScrY(MC_TRANSMUTE_Y + MC_SLOT);
		int w  = x1 - x0;
		int h  = y1 - y0;
		PackedCol border = PackedCol_Make(255, 255, 255, 70);
		PackedCol fill   = PackedCol_Make(255, 255, 255, 20);
		if (w > 0 && h > 0) {
			CCST_Draw2D_Flat(&ccst_flatVb, x0, y0, w, h, fill);
			CCST_Draw2D_Flat(&ccst_flatVb, x0, y0, w, 1, border);
			CCST_Draw2D_Flat(&ccst_flatVb, x0, y1 - 1, w, 1, border);
			CCST_Draw2D_Flat(&ccst_flatVb, x0, y0, 1, h, border);
			CCST_Draw2D_Flat(&ccst_flatVb, x1 - 1, y0, 1, h, border);
		}
	}

	CCST_Inv_DrawSlotHoverHighlight(MC_TRANSMUTE_X, MC_TRANSMUTE_Y,
		ccst_hover_kind == 1 && ccst_hover_slot == CCST_SLOT_TRANSMUTE);

	for (row = 0; row < 3; row++) {
		for (col = 0; col < 9; col++) {
			int mcx = 8 + col * MC_STEP;
			int mcy = MC_GRID_Y + row * MC_STEP;
			slot = 9 + col + row * 9;
			CCST_Inv_DrawSlotHoverHighlight(mcx, mcy, ccst_hover_kind == 1 && ccst_hover_slot == slot);
		}
	}
	for (col = 0; col < 9; col++) {
		int mcx = 8 + col * MC_STEP;
		CCST_Inv_DrawSlotHoverHighlight(mcx, MC_HOTBAR_Y, ccst_hover_kind == 1 && ccst_hover_slot == col);
	}

	vis = CCST_Inv_ListVisibleRows();
	for (i = 0; i < vis; i++) {
		int mcy = MC_LIST_TOP + i * MC_STEP;
		int idx = ccst_list_scroll + i;
		cc_bool hi = (ccst_hover_kind == 2 && ccst_hover_list_row == i);
		if (idx < ccst_mut_count && hi) {
			int rx, ry, rw, rh;
			CCST_Inv_ListGetContentRectPx(&rx, NULL, &rw, NULL);
			ry = CCST_Inv_ScrY(mcy + 1);
			rh = CCST_Inv_ScrY(mcy + 1 + MC_SLOT) - ry;
			if (rh < 1) rh = 1;
			CCST_Draw2D_Gradient(&ccst_flatVb, rx, ry, rw, rh, CCST_SEL_GRAD_TOP, CCST_SEL_GRAD_BOT);
		}
	}

	/* Scrollbar last so row highlight never draws over it */
	if (CCST_Inv_ListHasScrollbar()) {
		CCST_Inv_ListSyncScrollbarLayout();
		CCST_Inv_ScrollbarRender(&ccst_inv_listScroll);
		CCST_Inv_ListApplyScrollbarTopRow();
	}

	if (!ccst_inv_vb)
		ccst_inv_vb = Gfx_CreateDynamicVb(VERTEX_FORMAT_TEXTURED, CCST_INV_ICON_MAX_VERTS);

	ccst_drawer_saved = Drawer;
	verts = (struct VertexTextured*)Gfx_LockDynamicVb(ccst_inv_vb, VERTEX_FORMAT_TEXTURED, CCST_INV_ICON_MAX_VERTS);
	v = verts;
	CCST_Iso_BeginBatch(v, ccst_inv_tex_state);

	/* Transmute slot icon */
	{
		BlockID blk = ccst_transmute;
		if (blk != BLOCK_AIR && Blocks.Draw[blk] != DRAW_GAS) {
			cx = CCST_Inv_ScrX(MC_TRANSMUTE_X + MC_SLOT / 2);
			cy = CCST_Inv_ScrY(MC_TRANSMUTE_Y + MC_SLOT / 2);
			CCST_Iso_AddBatch(&v, blk, half, (float)cx, (float)cy);
		}
	}

	/* Main + hotbar (same row index as highlights, hit test, stack counts: row 0 = mcy 84). */
	for (row = 0; row < 3; row++) {
		for (col = 0; col < 9; col++) {
			BlockID blk;

			slot = 9 + col + row * 9;
			blk  = ccst_slots[slot];
			if (blk == BLOCK_AIR || Blocks.Draw[blk] == DRAW_GAS) continue;

			cx = CCST_Inv_ScrX(8 + col * MC_STEP + MC_SLOT / 2);
			cy = CCST_Inv_ScrY(MC_GRID_Y + row * MC_STEP + MC_SLOT / 2);
			CCST_Iso_AddBatch(&v, blk, half, (float)cx, (float)cy);
		}
	}
	for (col = 0; col < 9; col++) {
		BlockID blk;

		slot = col;
		blk  = ccst_slots[slot];
		if (blk == BLOCK_AIR || Blocks.Draw[blk] == DRAW_GAS) continue;

		cx = CCST_Inv_ScrX(8 + col * MC_STEP + MC_SLOT / 2);
		cy = CCST_Inv_ScrY(MC_HOTBAR_Y + MC_SLOT / 2);
		CCST_Iso_AddBatch(&v, blk, half, (float)cx, (float)cy);
	}

	/* List icons, left-aligned within the row so the remaining width can host the
	   recipe "m:n -> K" preview text (drawn after the iso batch so it layers on top). */
	for (i = 0; i < vis; i++) {
		int idx = ccst_list_scroll + i;
		BlockID blk;

		if (idx >= ccst_mut_count) break;
		blk = ccst_mut_blocks[idx];
		cx = CCST_Inv_ScrX(listMcX + MC_LIST_ICON_W / 2);
		cy = CCST_Inv_ScrY(MC_LIST_TOP + i * MC_STEP + MC_SLOT / 2);
		CCST_Iso_AddBatch(&v, blk, half * 0.92f, (float)cx, (float)cy);
	}

	/* Cursor stack */
	if (ccst_cursor != BLOCK_AIR && Blocks.Draw[ccst_cursor] != DRAW_GAS) {
		BlockID blk = ccst_cursor;
		cx = Pointers[0].x;
		cy = Pointers[0].y;
		CCST_Iso_AddBatch(&v, blk, half * 1.05f, (float)cx, (float)cy);
	}

	Drawer = ccst_drawer_saved;

	count = (int)(v - verts);
	Gfx_UnlockDynamicVb(ccst_inv_vb);

	Gfx_SetTexturing(true);
	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	Gfx_BindIb(Gfx.DefaultIb);
	Gfx_BindDynamicVb(ccst_inv_vb);
	CCST_Iso_Render(count, 0, ccst_inv_tex_state);

	if (ccst_transmute_count > 1 && ccst_transmute != BLOCK_AIR && Blocks.Draw[ccst_transmute] != DRAW_GAS) {
		sL = CCST_Inv_ScrX(MC_TRANSMUTE_X);
		sT = CCST_Inv_ScrY(MC_TRANSMUTE_Y);
		sW = CCST_Inv_ScrX(MC_TRANSMUTE_X + MC_SLOT) - sL;
		sH = CCST_Inv_ScrY(MC_TRANSMUTE_Y + MC_SLOT) - sT;
		CCST_Inv_DrawStackCountInItemSlot(ccst_transmute_count, sL, sT, sW, sH);
	}

	for (row = 0; row < 3; row++) {
		for (col = 0; col < 9; col++) {
			int mcx = 8 + col * MC_STEP;
			int mcy = MC_GRID_Y + row * MC_STEP;
			slot = 9 + col + row * 9;
			sc = ccst_slot_count[slot];
			if (sc > 1) {
				sL = CCST_Inv_ScrX(mcx);
				sT = CCST_Inv_ScrY(mcy);
				sW = CCST_Inv_ScrX(mcx + MC_SLOT) - sL;
				sH = CCST_Inv_ScrY(mcy + MC_SLOT) - sT;
				CCST_Inv_DrawStackCountInItemSlot(sc, sL, sT, sW, sH);
			}
		}
	}
	/*
	 * Per-row recipe preview. Format:
	 *   "m:n" , the canonical per-pair integer ratio (from blocktype.c TransmuteRatio),
	 *   "->K"  , expected yield for the current transmute-slot stack (floor of the
	 *            probabilistic partial, so the displayed number is a guaranteed floor;
	 *            a chance-based +1 may occur at click time for output-heavy pairs).
	 * Rows where the preview yield is 0 get a translucent dim overlay so the player can
	 * still see the target block but instantly knows it's unaffordable right now.
	 */
	{
		BlockID from_blk = ccst_transmute;
		int from_cnt     = ccst_transmute_count;
		for (i = 0; i < vis; i++) {
			int idx = ccst_list_scroll + i;
			int mcy = MC_LIST_TOP + i * MC_STEP;
			int textX, topY, botY, fontPt, room;
			int m = 0, n = 0, yield = 0, used = 0;
			cc_bool has_ratio;
			cc_string top, bot;
			char topBuf[16], botBuf[16];
			BlockID to_blk;

			if (idx >= ccst_mut_count) break;
			to_blk = ccst_mut_blocks[idx];

			if (from_blk == BLOCK_AIR) continue;

			has_ratio = CCST_BlockType_TransmuteRatio(from_blk, to_blk, &m, &n);
			if (!has_ratio) continue;

			/*
			 * Preview yields are reported against the cursor-room cap: if the cursor is
			 * holding a *different* block type, room is 0 and every such row shows "->0",
			 * which is exactly right, the click would be rejected until the player drops
			 * the held stack.
			 */
			room = CCST_Inv_CursorRoomFor(to_blk);
			CCST_BlockType_TransmutePreview(from_blk, from_cnt, to_blk, room, &yield, &used);

			{
				int slotH_px = CCST_Inv_ScrY(MC_SLOT) - CCST_Inv_ScrY(0);
				/* slotH/4 ≈ half of slot-height stack-count font; fits two lines per row. */
				fontPt = CCST_Inv_ClampCountFontPt((slotH_px * 4 + 8) / 16);
			}
			textX = CCST_Inv_ScrX(listMcX + MC_LIST_ICON_W + 2);
			topY  = CCST_Inv_ScrY(mcy + 1);
			botY  = CCST_Inv_ScrY(mcy + MC_SLOT / 2);

			String_InitArray(top, topBuf);
			String_AppendInt(&top, m);
			String_AppendConst(&top, ":");
			String_AppendInt(&top, n);

			String_InitArray(bot, botBuf);
			String_AppendConst(&bot, yield > 0 ? "->" : "--");
			if (yield > 0) String_AppendInt(&bot, yield);

			if (yield > 0) {
				CCST_Inv_DrawLabelAt(&top, textX, topY, fontPt, PACKEDCOL_WHITE);
				CCST_Inv_DrawLabelAt(&bot, textX, botY, fontPt,
					PackedCol_Make(180, 255, 180, 255));
			} else {
				/* Unaffordable: draw the preview greyed and overlay a dim shade on the row. */
				int rx, ry, rw, rh;
				CCST_Inv_ListGetContentRectPx(&rx, NULL, &rw, NULL);
				ry = CCST_Inv_ScrY(mcy);
				rh = CCST_Inv_ScrY(mcy + MC_SLOT) - ry;
				CCST_Draw2D_Flat(&ccst_flatVb, rx, ry, rw, rh, PackedCol_Make(0, 0, 0, 120));
				Gfx_SetTexturing(true);
				Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
				CCST_Inv_DrawLabelAt(&top, textX, topY, fontPt,
					PackedCol_Make(160, 160, 160, 255));
				CCST_Inv_DrawLabelAt(&bot, textX, botY, fontPt,
					PackedCol_Make(160, 160, 160, 255));
			}
		}

	/* Section headings (simple cues) */
	{
		struct DrawTextArgs a;
		cc_string s1 = String_FromConst("Crafting");
		cc_string s2 = String_FromConst("Inventory");
		PackedCol col = PackedCol_Make(220, 220, 220, 255);

		CCST_Inv_EnsureHeadingFont();
		a.font = &ccst_inv_headingFont;
		a.useShadow = true;

		a.text = s1;
		CCST_Draw2D_Text(&ccst_labelQuadVb, &a, CCST_Inv_ScrX(8), CCST_Inv_ScrY(MC_LIST_TOP + 2), col);
		a.text = s2;
		CCST_Draw2D_Text(&ccst_labelQuadVb, &a, CCST_Inv_ScrX(8), CCST_Inv_ScrY(MC_GRID_Y - 12), col);
	}
	}

	for (col = 0; col < 9; col++) {
		int mcx = 8 + col * MC_STEP;
		slot = col;
		sc = ccst_slot_count[slot];
		if (sc > 1) {
			sL = CCST_Inv_ScrX(mcx);
			sT = CCST_Inv_ScrY(MC_HOTBAR_Y);
			sW = CCST_Inv_ScrX(mcx + MC_SLOT) - sL;
			sH = CCST_Inv_ScrY(MC_HOTBAR_Y + MC_SLOT) - sT;
			CCST_Inv_DrawStackCountInItemSlot(sc, sL, sT, sW, sH);
		}
	}

	if (ccst_cursor_count > 1 && ccst_cursor != BLOCK_AIR && Blocks.Draw[ccst_cursor] != DRAW_GAS) {
		int mx = Pointers[0].x;
		int my = Pointers[0].y;
		ch = half * 1.05f;
		sL = (int)((float)mx - ch + 0.5f);
		sT = (int)((float)my - ch + 0.5f);
		sW = (int)(2.0f * ch + 0.5f);
		sH = (int)(2.0f * ch + 0.5f);
		CCST_Inv_DrawStackCountInItemSlot(ccst_cursor_count, sL, sT, sW, sH);
	}
}

void CCST_Inv_Init(void) {
	int i;

	CCST_InvGrab_InitOnce();
	ccst_inv_open = false;
	ccst_transmute = BLOCK_AIR;
	ccst_cursor = BLOCK_AIR;
	ccst_list_scroll = 0;
	ccst_mut_count = 0;

	for (i = 0; i < CCST_INV_SLOTS; i++) {
		ccst_slots[i] = BLOCK_AIR;
		ccst_slot_count[i] = 0;
	}
	ccst_transmute_count = 0;
	ccst_cursor_count = 0;

	if (EventAPIVersion >= 4)
		Event_Register_(&InputEvents.Down2, NULL, CCST_Inv_OnInputDown2);
	Event_Register_(&PointerEvents.Down, NULL, CCST_Inv_OnPointerDown);
	Event_Register_(&PointerEvents.Up, NULL, CCST_Inv_OnPointerUp);
	Event_Register_(&PointerEvents.Moved, NULL, CCST_Inv_OnPointerMoved);
	Event_Register_(&InputEvents.Wheel, NULL, CCST_Inv_OnWheel);
	Event_Register_(&UserEvents.BlockChanged, NULL, CCST_Inv_OnUserBlockChanged);
}

void CCST_Inv_Free(void) {
	if (EventAPIVersion >= 4)
		Event_Unregister_(&InputEvents.Down2, NULL, CCST_Inv_OnInputDown2);
	Event_Unregister_(&PointerEvents.Down, NULL, CCST_Inv_OnPointerDown);
	Event_Unregister_(&PointerEvents.Up, NULL, CCST_Inv_OnPointerUp);
	Event_Unregister_(&PointerEvents.Moved, NULL, CCST_Inv_OnPointerMoved);
	Event_Unregister_(&InputEvents.Wheel, NULL, CCST_Inv_OnWheel);
	Event_Unregister_(&UserEvents.BlockChanged, NULL, CCST_Inv_OnUserBlockChanged);

	Gfx_DeleteDynamicVb(&ccst_inv_vb);
	ccst_inv_vb = 0;
	Gfx_DeleteDynamicVb(&ccst_flatVb);
	ccst_flatVb = 0;
	Gfx_DeleteDynamicVb(&ccst_labelQuadVb);
	ccst_labelQuadVb = 0;
	CCST_Font_FreeCached(&ccst_inv_countFont, &ccst_inv_countFontPt);
	CCST_Font_FreeCached(&ccst_inv_titleFont, &ccst_inv_titleFontPt);
	CCST_Font_FreeCached(&ccst_inv_headingFont, &ccst_inv_headingFontPt);
	if (ccst_inv_open)
		CCST_Inv_CloseInternal();
	ccst_inv_open = false;
}
