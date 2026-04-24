/* Shared 2D draw helpers for plugin UIs.
 *
 * ClassiCube does not export some convenience helpers to dlopen'd plugins, so several modules
 * duplicated "draw flat rect", "draw vertical gradient rect", and "draw textured quad" logic.
 * Centralise that here to reduce drift.
 */
#ifndef CCST_DRAW2D_H
#define CCST_DRAW2D_H

#include "Core.h"
#include "Graphics.h"
#include "PackedCol.h"

static CC_INLINE void CCST_Draw2D_Flat(GfxResourceID* vb, int x, int y, int width, int height, PackedCol color) {
	struct VertexColoured* v;

	if (!vb) return;
	if (width < 1 || height < 1) return;
	if (!*vb) *vb = Gfx_CreateDynamicVb(VERTEX_FORMAT_COLOURED, 4);
	if (!*vb) return;

	Gfx_SetTexturing(false);
	Gfx_SetVertexFormat(VERTEX_FORMAT_COLOURED);
	v = (struct VertexColoured*)Gfx_LockDynamicVb(*vb, VERTEX_FORMAT_COLOURED, 4);
	v[0].x = (float)x;           v[0].y = (float)y;            v[0].z = 0; v[0].Col = color;
	v[1].x = (float)(x + width); v[1].y = (float)y;            v[1].z = 0; v[1].Col = color;
	v[2].x = (float)(x + width); v[2].y = (float)(y + height); v[2].z = 0; v[2].Col = color;
	v[3].x = (float)x;           v[3].y = (float)(y + height); v[3].z = 0; v[3].Col = color;
	Gfx_UnlockDynamicVb(*vb);
	Gfx_BindIb(Gfx.DefaultIb);
	Gfx_BindDynamicVb(*vb);
	Gfx_DrawVb_IndexedTris_Range(4, 0, DRAW_HINT_RECT);
}

static CC_INLINE void CCST_Draw2D_Gradient(GfxResourceID* vb, int x, int y, int width, int height, PackedCol top, PackedCol bottom) {
	struct VertexColoured* v;

	if (!vb) return;
	if (width < 1 || height < 1) return;
	if (!*vb) *vb = Gfx_CreateDynamicVb(VERTEX_FORMAT_COLOURED, 4);
	if (!*vb) return;

	Gfx_SetTexturing(false);
	Gfx_SetVertexFormat(VERTEX_FORMAT_COLOURED);
	v = (struct VertexColoured*)Gfx_LockDynamicVb(*vb, VERTEX_FORMAT_COLOURED, 4);
	v[0].x = (float)x;           v[0].y = (float)y;            v[0].z = 0; v[0].Col = top;
	v[1].x = (float)(x + width); v[1].y = (float)y;            v[1].z = 0; v[1].Col = top;
	v[2].x = (float)(x + width); v[2].y = (float)(y + height); v[2].z = 0; v[2].Col = bottom;
	v[3].x = (float)x;           v[3].y = (float)(y + height); v[3].z = 0; v[3].Col = bottom;
	Gfx_UnlockDynamicVb(*vb);
	Gfx_BindIb(Gfx.DefaultIb);
	Gfx_BindDynamicVb(*vb);
	Gfx_DrawVb_IndexedTris_Range(4, 0, DRAW_HINT_RECT);
}

static CC_INLINE void CCST_Draw2D_TexturedQuad(GfxResourceID* vb, const struct Texture* t, PackedCol color) {
	struct VertexTextured* v;
	float x1, x2, y1, y2;

	if (!vb || !t || !t->ID) return;
	if (!*vb) *vb = Gfx_CreateDynamicVb(VERTEX_FORMAT_TEXTURED, 4);
	if (!*vb) return;

	x1 = (float)t->x;
	x2 = (float)t->x + (float)t->width;
	y1 = (float)t->y;
	y2 = (float)t->y + (float)t->height;

	Gfx_SetTexturing(true);
	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	v = (struct VertexTextured*)Gfx_LockDynamicVb(*vb, VERTEX_FORMAT_TEXTURED, 4);
	v[0].x = x1; v[0].y = y1; v[0].z = 0; v[0].Col = color; v[0].U = t->uv.u1; v[0].V = t->uv.v1;
	v[1].x = x2; v[1].y = y1; v[1].z = 0; v[1].Col = color; v[1].U = t->uv.u2; v[1].V = t->uv.v1;
	v[2].x = x2; v[2].y = y2; v[2].z = 0; v[2].Col = color; v[2].U = t->uv.u2; v[2].V = t->uv.v2;
	v[3].x = x1; v[3].y = y2; v[3].z = 0; v[3].Col = color; v[3].U = t->uv.u1; v[3].V = t->uv.v2;
	Gfx_UnlockDynamicVb(*vb);
	Gfx_BindTexture(t->ID);
	Gfx_BindDynamicVb(*vb);
	Gfx_BindIb(Gfx.DefaultIb);
	Gfx_DrawVb_IndexedTris_Range(4, 0, DRAW_HINT_SPRITE);
}

#endif
