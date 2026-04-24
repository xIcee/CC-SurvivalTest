/*
    icons.c
    survival hud overlays.

    - health hearts and air bubbles
    - score line and stack count rendering
    - low-hp jitter and damage flash
*/
#include "Funcs.h"
#include "Game.h"
#include "Graphics.h"
#include "Gui.h"
#include "ExtMath.h"
#include "PackedCol.h"
#include "Window.h"
#include <math.h>
#include "Drawer2D.h"
#include "String_.h"
#include "deathscreen.h"
#include "fakeinventory.h"
#include "health.h"
#include "icons.h"
#include "score.h"
#include "ccst_ui_scale.h"
#include "ccst_font.h"
#include "ccst_drawtext.h"

#define ICON_ATLAS_PX_W 256.0f
#define ICON_ATLAS_PX_H 64.0f

#define HEART_U_EMPTY 16
#define HEART_U_FULL  52
#define HEART_U_HALF  61
#define HEART_U_WHITE_FULL  70
#define HEART_U_WHITE_HALF  79
#define HEART_SIZE    9

#define AIR_U_FULL   16
#define AIR_U_BREAK  25
#define AIR_V        18
#define AIR_SIZE     9

#define JAVA_HEART_STRIDE        8
#define JAVA_AIR_ABOVE_HEART_ROW    9
#define JAVA_HEART_GAP_ABOVE_HOTBAR  1
#define JAVA_HUD_JITTER_SEED_MUL 312871 
#define JAVA_HOTBAR_TEX_W 182
#define JAVA_HOTBAR_TEX_H 22
#define JAVA_ARROWS_DX_FROM_SCREEN_CENTER 8
#define CCST_SCORE_FONT_PT_SCALE 8.0f
#define CCST_SCORE_FONT_PT_MAX   48

#define CCST_ICON_MAX_VERTS 256

static GfxResourceID ccst_iconVb;
static int ccst_hud_frame_counter; 
static struct FontDesc ccst_score_font;
static int ccst_score_font_pt = -1;

// compute hotbar-relative layout
static void CCST_Icon_HotbarGetLayout(int* outX, int* outY, int* outH, float* outScaleX, float* outScaleY) {
	float hb, sx, sy;
	int w, h;

	hb = CCST_UIHotbarScaleLinear();
	sx = hb * DisplayInfo.ScaleX;
	sy = hb * DisplayInfo.ScaleY;
	w  = (int)(JAVA_HOTBAR_TEX_W * sx);
	h  = (int)floorf(22.0f * sy);
	*outX = (Game.Width - w) / 2;
	*outY = Game.Height - h;
	*outH = h;
	*outScaleX = sx;
	*outScaleY = sy;
}

static void CCST_Make2DQuad(const struct Texture* tex, PackedCol color, struct VertexTextured** vertices) {
	float x1 = (float)tex->x, x2 = (float)(tex->x + tex->width);
	float y1 = (float)tex->y, y2 = (float)(tex->y + tex->height);
	struct VertexTextured* v = *vertices;

#if defined CC_BUILD_PSP
	v[0].Col = color; v[0].U = tex->uv.u1; v[0].V = tex->uv.v1; v[0].x = x1; v[0].y = y1; v[0].z = 0;
	v[1].Col = color; v[1].U = tex->uv.u2; v[1].V = tex->uv.v1; v[1].x = x2; v[1].y = y1; v[1].z = 0;
	v[2].Col = color; v[2].U = tex->uv.u2; v[2].V = tex->uv.v2; v[2].x = x2; v[2].y = y2; v[2].z = 0;
	v[3].Col = color; v[3].U = tex->uv.u1; v[3].V = tex->uv.v2; v[3].x = x1; v[3].y = y2; v[3].z = 0;
#else
	v[0].x = x1; v[0].y = y1; v[0].z = 0; v[0].Col = color; v[0].U = tex->uv.u1; v[0].V = tex->uv.v1;
	v[1].x = x2; v[1].y = y1; v[1].z = 0; v[1].Col = color; v[1].U = tex->uv.u2; v[1].V = tex->uv.v1;
	v[2].x = x2; v[2].y = y2; v[2].z = 0; v[2].Col = color; v[2].U = tex->uv.u2; v[2].V = tex->uv.v2;
	v[3].x = x1; v[3].y = y2; v[3].z = 0; v[3].Col = color; v[3].U = tex->uv.u1; v[3].V = tex->uv.v2;
#endif
	*vertices = v + 4;
}

static void CCST_Icon_SetUV(struct Texture* t, int u, int v, int w, int h) {
	t->uv.u1 = u / ICON_ATLAS_PX_W;
	t->uv.v1 = v / ICON_ATLAS_PX_H;
	t->uv.u2 = (u + w) / ICON_ATLAS_PX_W;
	t->uv.v2 = (v + h) / ICON_ATLAS_PX_H;
}

static void CCST_Icon_Quad(struct VertexTextured** ptr, int sx, int sy, int au, int av, int atlasW, int atlasH, int drawW, int drawH) {
	struct Texture tex;

	tex.ID = 0;
	tex.x = (short)sx;
	tex.y = (short)sy;
	tex.width = (cc_uint16)drawW;
	tex.height = (cc_uint16)drawH;
	CCST_Icon_SetUV(&tex, au, av, atlasW, atlasH);
	CCST_Make2DQuad(&tex, PACKEDCOL_WHITE, ptr);
}

static void CCST_Icons_EnsureScoreFont(int point) {
	CCST_Font_EnsurePoint(&ccst_score_font, &ccst_score_font_pt,
		point, 8, CCST_SCORE_FONT_PT_MAX, FONT_FLAGS_NONE);
}

// draw the survival score line
static void CCST_Icons_DrawScoreLine(float scaleX, float scaleY, int heartY, int iconH) {
	cc_string str;
	char buf[STRING_SIZE];
	struct DrawTextArgs args;
	int fontPt, sx, sy;

	if (Gfx.LostContext) return;

	fontPt = (int)(CCST_SCORE_FONT_PT_SCALE * scaleY + 0.5f);
	CCST_Icons_EnsureScoreFont(fontPt);

	String_InitArray(str, buf);
	String_AppendConst(&str, "Score: &e");
	String_AppendInt(&str, CCST_Score_Get());
	args.text = str;
	args.font = &ccst_score_font;
	args.useShadow = true;
	sx = Game.Width / 2 + (int)(JAVA_ARROWS_DX_FROM_SCREEN_CENTER * scaleX);
	sy = heartY + iconH;
	if (sy < 2) sy = 2;

	CCST_Draw2D_TextBottomAlignedClampedX(&ccst_iconVb, &args, sx, sy, 2, Game.Width - 2, PACKEDCOL_WHITE);
}

void CCST_Icons_Free(void) {
	Gfx_DeleteDynamicVb(&ccst_iconVb);
	CCST_Font_FreeCached(&ccst_score_font, &ccst_score_font_pt);
}

void CCST_Icons_ContextLost(void) {
	Gfx_DeleteDynamicVb(&ccst_iconVb);
}

// draw hearts and bubbles
void CCST_Icons_Draw2D(float delta) {
	struct VertexTextured* verts;
	struct VertexTextured** ptr;
	RNGState rnd;
	int i, hx, n, air;
	int i12, i13, i14;
	int inv, lastH, invFlash, halfIdx, hp;
	int jitterUnit;
	float scaleX, scaleY;
	int hbX, hbY, hbH;
	int stride, iconW, iconH, heartY, airY;

	CCST_Health_OnFrame(delta);

	if (!CCST_Health_IsSurvivalActive()) return;
	if (CCST_Health_IsDeadOrDying()) return;
	if (!Gui.IconsTex) return;

	ccst_hud_frame_counter++;
	CCST_Icon_HotbarGetLayout(&hbX, &hbY, &hbH, &scaleX, &scaleY);
	(void)hbH;
	stride = (int)(JAVA_HEART_STRIDE * scaleX + 0.5f);
	iconW  = (int)(HEART_SIZE * scaleX + 0.5f);
	iconH  = (int)(HEART_SIZE * scaleY + 0.5f);
	if (iconW < 1) iconW = 1;
	if (iconH < 1) iconH = 1;

	heartY = hbY - iconH - (int)(JAVA_HEART_GAP_ABOVE_HOTBAR * scaleY + 0.5f);
	airY   = heartY - (int)(JAVA_AIR_ABOVE_HEART_ROW * scaleY + 0.5f);
	jitterUnit = (int)ceilf(scaleY);
	if (jitterUnit < 1) jitterUnit = 1;

	if (!ccst_iconVb)
		ccst_iconVb = Gfx_CreateDynamicVb(VERTEX_FORMAT_TEXTURED, CCST_ICON_MAX_VERTS);

	verts = (struct VertexTextured*)Gfx_LockDynamicVb(ccst_iconVb, VERTEX_FORMAT_TEXTURED, CCST_ICON_MAX_VERTS);
	ptr = &verts;
	n = 0;

	inv     = CCST_Health_GetInvulnTicks();
	lastH   = CCST_Health_GetLastHealth();
	invFlash = inv >= 10 && ((inv / 3) % 2) == 1;
	Random_Seed(&rnd, (int)((cc_int64)ccst_hud_frame_counter * JAVA_HUD_JITTER_SEED_MUL));

	hp = CCST_Health_Get();
	for (i = 0; i < 10; i++) {
		int sx = hbX + i * stride;
		int sy = heartY;
		int bgU = HEART_U_EMPTY + (invFlash ? 9 : 0);

		hx = hp - i * 2;
		halfIdx = i * 2 + 1;

		if (hp <= 4 && Random_Next(&rnd, 2))
			sy += jitterUnit;

		CCST_Icon_Quad(ptr, sx, sy, bgU, 0, HEART_SIZE, HEART_SIZE, iconW, iconH);
		n += 4;

		if (invFlash && lastH > hp) {
			if (halfIdx < lastH) {
				CCST_Icon_Quad(ptr, sx, sy, HEART_U_WHITE_FULL, 0, HEART_SIZE, HEART_SIZE, iconW, iconH);
				n += 4;
			} else if (halfIdx == lastH) {
				CCST_Icon_Quad(ptr, sx, sy, HEART_U_WHITE_HALF, 0, HEART_SIZE, HEART_SIZE, iconW, iconH);
				n += 4;
			}
		}

		if (hx >= 2) {
			CCST_Icon_Quad(ptr, sx, sy, HEART_U_FULL, 0, HEART_SIZE, HEART_SIZE, iconW, iconH);
			n += 4;
		} else if (hx == 1) {
			CCST_Icon_Quad(ptr, sx, sy, HEART_U_HALF, 0, HEART_SIZE, HEART_SIZE, iconW, iconH);
			n += 4;
		}
	}

	air = CCST_Air_Get();
	if (air < CCST_AIR_MAX) {
		i12 = (int)ceilf((float)(air - 2) * 10.0f / 300.0f);
		i13 = (int)ceilf((float)air * 10.0f / 300.0f) - i12;
		for (i14 = 0; i14 < i12 + i13; i14++) {
			int u = i14 < i12 ? AIR_U_FULL : AIR_U_BREAK;
			int sx = hbX + i14 * stride;
			int sy = airY;

			CCST_Icon_Quad(ptr, sx, sy, u, AIR_V, AIR_SIZE, AIR_SIZE, iconW, iconH);
			n += 4;
		}
	}

	Gfx_UnlockDynamicVb(ccst_iconVb);

	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	Gfx_BindTexture(Gui.IconsTex);
	Gfx_BindDynamicVb(ccst_iconVb);
	Gfx_DrawVb_IndexedTris_Range(n, 0, DRAW_HINT_SPRITE);

	CCST_Icons_DrawScoreLine(scaleX, scaleY, heartY, iconH);
	CCST_Inv_DrawHotbarStackCounts2D();
}
