/*
 * Reimplementation of ClassiCube IsometricDrawer.c for the survival layer plugin.
 */
#include "isoinv.h"
#include "Block.h"
#include "Constants.h"
#include "Core.h"
#include "Drawer.h"
#include "ExtMath.h"
#include "Graphics.h"
#include "Options.h"
#include "PackedCol.h"
#include "TexturePack.h"
#include <math.h>

#if CC_BUILD_FPU_MODE <= CC_FPU_MODE_MINIMAL
	#define CCST_ISO_DRAW_HINT DRAW_HINT_SPRITE
#else
	#define CCST_ISO_DRAW_HINT DRAW_HINT_NONE
#endif

static struct VertexTextured* ccst_iso_v;
static int* ccst_iso_st;

static cc_bool ccst_iso_cacheInited;
static PackedCol ccst_iso_colorXSide, ccst_iso_colorZSide, ccst_iso_colorYBottom;
static float ccst_iso_posX, ccst_iso_posY;

#define CCST_ISO_COSX  (0.86602540378443864f)
#define CCST_ISO_SINX  (0.50000000000000000f)
#define CCST_ISO_COSY  (0.70710678118654752f)
#define CCST_ISO_SINY (-0.70710678118654752f)

static void CCST_Iso_InitCache(void) {
	if (ccst_iso_cacheInited) return;
	ccst_iso_cacheInited = true;
	/* PackedCol_GetShaded is not exported; same as PackedCol.c using CC_API PackedCol_Scale */
	ccst_iso_colorXSide   = PackedCol_Scale(PACKEDCOL_WHITE, PACKEDCOL_SHADE_X);
	ccst_iso_colorZSide   = PackedCol_Scale(PACKEDCOL_WHITE, PACKEDCOL_SHADE_Z);
	ccst_iso_colorYBottom = PackedCol_Scale(PACKEDCOL_WHITE, PACKEDCOL_SHADE_YMIN);
}

/* Atlas1D_TexRec is not exported to plugins; keep in sync with TexturePack.c */
static TextureRec CCST_Atlas1D_TexRec(TextureLoc texLoc, int uCount, int* index) {
	TextureRec rec;
	int y = Atlas1D_RowId(texLoc);
	*index = Atlas1D_Index(texLoc);
	rec.u1 = 0.0f;
	rec.v1 = y * Atlas1D.InvTileSize;
	rec.u2 = (uCount - 1) + UV2_Scale;
	rec.v2 = rec.v1 + UV2_Scale * Atlas1D.InvTileSize;
	return rec;
}

static TextureLoc CCST_Iso_PushTexState(TextureLoc loc) {
	TextureLoc ret = loc;
	*ccst_iso_st++ = Atlas1D_Index(loc);
	return ret;
}

static void CCST_Iso_Flat(BlockID block, float size) {
	int texIndex;
	TextureLoc loc = Block_Tex(block, FACE_ZMAX);
	TextureRec rec = CCST_Atlas1D_TexRec(loc, 1, &texIndex);
	struct VertexTextured* v;
	float minX, maxX, minY, maxY;
	PackedCol color;
	float scale;

	*ccst_iso_st++ = texIndex;
	color = PACKEDCOL_WHITE;
	Block_Tint(color, block);

	/* Game_ClassicMode is not exported to plugins; OPT_CLASSIC_MODE matches Game.c LoadOptions. */
	scale = Options_GetBool(OPT_CLASSIC_MODE, false) ? 0.70f : 0.88f;
	size  = (float)(int)ceilf((double)(size * scale));
	minX  = ccst_iso_posX - size; maxX = ccst_iso_posX + size;
	minY  = ccst_iso_posY - size; maxY = ccst_iso_posY + size;

	v = ccst_iso_v;
	v->x = minX; v->y = minY; v->z = 0; v->Col = color; v->U = rec.u1; v->V = rec.v1; v++;
	v->x = maxX; v->y = minY; v->z = 0; v->Col = color; v->U = rec.u2; v->V = rec.v1; v++;
	v->x = maxX; v->y = maxY; v->z = 0; v->Col = color; v->U = rec.u2; v->V = rec.v2; v++;
	v->x = minX; v->y = maxY; v->z = 0; v->Col = color; v->U = rec.u1; v->V = rec.v2; v++;
	ccst_iso_v = v;
}

static void CCST_Iso_Angled(BlockID block, float size) {
	cc_bool bright;
	struct VertexTextured* beg = ccst_iso_v;
	struct VertexTextured* v;
	float x, y, scale;

	scale = size / (2.0f * CCST_ISO_COSY);

	Drawer.MinBB = Blocks.MinBB[block]; Drawer.MinBB.y = 1.0f - Drawer.MinBB.y;
	Drawer.MaxBB = Blocks.MaxBB[block]; Drawer.MaxBB.y = 1.0f - Drawer.MaxBB.y;

	Drawer.X1 = scale * (1.0f - Blocks.MinBB[block].x * 2.0f);
	Drawer.X2 = scale * (1.0f - Blocks.MaxBB[block].x * 2.0f);
	Drawer.Y1 = scale * (1.0f - Blocks.MinBB[block].y * 2.0f);
	Drawer.Y2 = scale * (1.0f - Blocks.MaxBB[block].y * 2.0f);
	Drawer.Z1 = scale * (1.0f - Blocks.MinBB[block].z * 2.0f);
	Drawer.Z2 = scale * (1.0f - Blocks.MaxBB[block].z * 2.0f);

	bright = Blocks.Brightness[block];
	Drawer.Tinted  = Blocks.Tinted[block];
	Drawer.TintCol = Blocks.FogCol[block];

	Drawer_XMax(1, bright ? PACKEDCOL_WHITE : ccst_iso_colorXSide,
		CCST_Iso_PushTexState(Block_Tex(block, FACE_XMAX)), &ccst_iso_v);
	Drawer_ZMin(1, bright ? PACKEDCOL_WHITE : ccst_iso_colorZSide,
		CCST_Iso_PushTexState(Block_Tex(block, FACE_ZMIN)), &ccst_iso_v);
	Drawer_YMax(1, PACKEDCOL_WHITE,
		CCST_Iso_PushTexState(Block_Tex(block, FACE_YMAX)), &ccst_iso_v);

	for (v = beg; v < ccst_iso_v; v++) {
		x = v->x * CCST_ISO_COSY                              + v->z * -CCST_ISO_SINY;
		y = v->x * CCST_ISO_SINX * CCST_ISO_SINY + v->y * CCST_ISO_COSX + v->z * CCST_ISO_SINX * CCST_ISO_COSY;
		v->x = x + ccst_iso_posX;
		v->y = y + ccst_iso_posY;
	}
}

void CCST_Iso_BeginBatch(struct VertexTextured* vertices, int* statePerQuad) {
	CCST_Iso_InitCache();
	ccst_iso_v  = vertices;
	ccst_iso_st = statePerQuad;
}

void CCST_Iso_AddBatch(struct VertexTextured** vertices, BlockID block, float size, float x, float y) {
	if (Blocks.Draw[block] == DRAW_GAS) return;

	ccst_iso_v = *vertices;
	ccst_iso_posX = x;
	ccst_iso_posY = y;

#if CC_BUILD_FPU_MODE <= CC_FPU_MODE_MINIMAL
	CCST_Iso_Flat(block, size);
#else
	if (Blocks.Draw[block] == DRAW_SPRITE) {
		CCST_Iso_Flat(block, size);
	} else {
		CCST_Iso_Angled(block, size);
	}
#endif
	*vertices = ccst_iso_v;
}

void CCST_Iso_Render(int count, int offset, int* statePerQuad) {
	int i, curIdx, batchBeg, batchLen;

	if (count <= 0) return;

	curIdx   = statePerQuad[0];
	batchLen = 0;
	batchBeg = offset;

	for (i = 0; i < count / 4; i++, batchLen += 4) {
		if (statePerQuad[i] == curIdx) continue;

		Gfx_BindTexture(Atlas1D.TexIds[curIdx]);
		Gfx_DrawVb_IndexedTris_Range(batchLen, batchBeg, CCST_ISO_DRAW_HINT);

		curIdx   = statePerQuad[i];
		batchBeg += batchLen;
		batchLen  = 0;
	}

	Gfx_BindTexture(Atlas1D.TexIds[curIdx]);
	Gfx_DrawVb_IndexedTris_Range(batchLen, batchBeg, CCST_ISO_DRAW_HINT);
}
