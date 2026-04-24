/* Shared "render text to texture, draw, delete" helper. */
#ifndef CCST_DRAWTEXT_H
#define CCST_DRAWTEXT_H

#include "ccst_draw2d.h"
#include "Drawer2D.h"

/* Renders `args` to a temporary texture, draws it at (x,y), then deletes it.
 * Uses the provided dynamic textured VB (created on demand).
 */
static CC_INLINE void CCST_Draw2D_Text(
	GfxResourceID* texturedQuadVb,
	struct DrawTextArgs* args,
	int x, int y,
	PackedCol color
) {
	struct Texture tex = { 0 };
	struct Texture drawTex;

	if (!texturedQuadVb || !args) return;
	if (Gfx.LostContext) return;

	Drawer2D_MakeTextTexture(&tex, args);
	if (!tex.ID) return;

	drawTex = tex;
	drawTex.x = (short)x;
	drawTex.y = (short)y;
	CCST_Draw2D_TexturedQuad(texturedQuadVb, &drawTex, color);
	Gfx_DeleteTexture(&tex.ID);
}

static CC_INLINE void CCST_Draw2D_TextRightAligned(
	GfxResourceID* texturedQuadVb,
	struct DrawTextArgs* args,
	int rightX, int y,
	PackedCol color
) {
	struct Texture tex = { 0 };
	struct Texture drawTex;

	if (!texturedQuadVb || !args) return;
	if (Gfx.LostContext) return;

	Drawer2D_MakeTextTexture(&tex, args);
	if (!tex.ID) return;

	drawTex = tex;
	drawTex.x = (short)(rightX - (int)tex.width);
	drawTex.y = (short)y;
	CCST_Draw2D_TexturedQuad(texturedQuadVb, &drawTex, color);
	Gfx_DeleteTexture(&tex.ID);
}

static CC_INLINE void CCST_Draw2D_TextCenteredX(
	GfxResourceID* texturedQuadVb,
	struct DrawTextArgs* args,
	int centerX, int y,
	PackedCol color
) {
	struct Texture tex = { 0 };
	struct Texture drawTex;

	if (!texturedQuadVb || !args) return;
	if (Gfx.LostContext) return;

	Drawer2D_MakeTextTexture(&tex, args);
	if (!tex.ID) return;

	drawTex = tex;
	drawTex.x = (short)(centerX - (int)tex.width / 2);
	drawTex.y = (short)y;
	CCST_Draw2D_TexturedQuad(texturedQuadVb, &drawTex, color);
	Gfx_DeleteTexture(&tex.ID);
}

static CC_INLINE void CCST_Draw2D_TextCenteredInRect(
	GfxResourceID* texturedQuadVb,
	struct DrawTextArgs* args,
	int rx, int ry, int rw, int rh,
	PackedCol color
) {
	struct Texture tex = { 0 };
	struct Texture drawTex;

	if (!texturedQuadVb || !args) return;
	if (Gfx.LostContext) return;
	if (rw < 1 || rh < 1) return;

	Drawer2D_MakeTextTexture(&tex, args);
	if (!tex.ID) return;

	drawTex = tex;
	drawTex.x = (short)(rx + (rw - (int)tex.width) / 2);
	drawTex.y = (short)(ry + (rh - (int)tex.height) / 2);
	CCST_Draw2D_TexturedQuad(texturedQuadVb, &drawTex, color);
	Gfx_DeleteTexture(&tex.ID);
}

/* Draws text at y, clamping x so the rendered texture stays within [minX, maxX]. */
static CC_INLINE void CCST_Draw2D_TextClampedX(
	GfxResourceID* texturedQuadVb,
	struct DrawTextArgs* args,
	int desiredX, int y,
	int minX, int maxX,
	PackedCol color
) {
	struct Texture tex = { 0 };
	struct Texture drawTex;
	int x;

	if (!texturedQuadVb || !args) return;
	if (Gfx.LostContext) return;

	Drawer2D_MakeTextTexture(&tex, args);
	if (!tex.ID) return;

	x = desiredX;
	if (x + (int)tex.width > maxX) x = maxX - (int)tex.width;
	if (x < minX) x = minX;

	drawTex = tex;
	drawTex.x = (short)x;
	drawTex.y = (short)y;
	CCST_Draw2D_TexturedQuad(texturedQuadVb, &drawTex, color);
	Gfx_DeleteTexture(&tex.ID);
}

/* Like TextClampedX, but aligns bottom of text texture to bottomY. */
static CC_INLINE void CCST_Draw2D_TextBottomAlignedClampedX(
	GfxResourceID* texturedQuadVb,
	struct DrawTextArgs* args,
	int desiredX, int bottomY,
	int minX, int maxX,
	PackedCol color
) {
	struct Texture tex = { 0 };
	struct Texture drawTex;
	int x, y;

	if (!texturedQuadVb || !args) return;
	if (Gfx.LostContext) return;

	Drawer2D_MakeTextTexture(&tex, args);
	if (!tex.ID) return;

	x = desiredX;
	if (x + (int)tex.width > maxX) x = maxX - (int)tex.width;
	if (x < minX) x = minX;
	y = bottomY - (int)tex.height;

	drawTex = tex;
	drawTex.x = (short)x;
	drawTex.y = (short)y;
	CCST_Draw2D_TexturedQuad(texturedQuadVb, &drawTex, color);
	Gfx_DeleteTexture(&tex.ID);
}

/* Draws scaled text texture centered horizontally at centerX. If out_unscaled_h is non-NULL,
 * writes the unscaled texture height (useful for spacing). */
static CC_INLINE void CCST_Draw2D_TextScaledCenteredX(
	GfxResourceID* texturedQuadVb,
	struct DrawTextArgs* args,
	int centerX, int y,
	int scale,
	PackedCol color,
	int* out_unscaled_h
) {
	struct Texture tex = { 0 };
	struct Texture drawTex;
	int w, h;

	if (!texturedQuadVb || !args) return;
	if (Gfx.LostContext) return;
	if (scale < 1) scale = 1;

	Drawer2D_MakeTextTexture(&tex, args);
	if (!tex.ID) return;

	if (out_unscaled_h) *out_unscaled_h = (int)tex.height;

	w = (int)tex.width * scale;
	h = (int)tex.height * scale;
	drawTex = tex;
	drawTex.width  = (cc_uint16)w;
	drawTex.height = (cc_uint16)h;
	drawTex.x = (short)(centerX - w / 2);
	drawTex.y = (short)y;
	CCST_Draw2D_TexturedQuad(texturedQuadVb, &drawTex, color);
	Gfx_DeleteTexture(&tex.ID);
}

#endif
