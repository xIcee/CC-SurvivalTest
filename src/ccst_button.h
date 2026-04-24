/* Shared button background renderer (matches ClassiCube Widgets.c ButtonWidget_Render).
 *
 * This is used by overlay UIs that need classic menu-style buttons but can't call the
 * internal non-exported helpers directly.
 */
#ifndef CCST_BUTTON_H
#define CCST_BUTTON_H

#include "Gui.h"
#include "PackedCol.h"
#include "ccst_draw2d.h"
#include "ccst_util.h"

/* Button texture UVs on gui.png/gui_classic.png (top half only). */
#define CCST_BTN_uWIDTH (200.0f / 256.0f)
#define CCST_ButtonUV(u1,v1, u2,v2) Tex_UV((u1)/256.0f,(v1)/128.0f, (u2)/256.0f,(v2)/128.0f)

static CC_INLINE void CCST_DrawButtonBackground(
	GfxResourceID* texturedQuadVb,
	int x, int y, int width, int height,
	cc_bool selected, cc_bool disabled
) {
	struct Texture back;
	float scale;

	static struct Texture shadowTex   = { 0, Tex_Rect(0,0, 0,0), CCST_ButtonUV(0,66, 200,86)  };
	static struct Texture selectedTex = { 0, Tex_Rect(0,0, 0,0), CCST_ButtonUV(0,86, 200,106) };
	static struct Texture disabledTex = { 0, Tex_Rect(0,0, 0,0), CCST_ButtonUV(0,46, 200,66)  };

	back = selected ? selectedTex : shadowTex;
	if (disabled) back = disabledTex;

	back.ID = Gui.ClassicTexture ? Gui.GuiClassicTex : Gui.GuiTex;
	if (!back.ID) return;

	back.x = (short)x;
	back.y = (short)y;
	back.width  = (cc_uint16)width;
	back.height = (cc_uint16)height;

	/* Match Widgets.c ButtonWidget_Render exactly. */
	if (width >= 400) {
		CCST_Draw2D_TexturedQuad(texturedQuadVb, &back, PACKEDCOL_WHITE);
	} else {
		/* Split button down the middle */
		scale = (width / 400.0f) / (2 * DisplayInfo.ScaleX);

		back.width = (cc_uint16)(width / 2);
		back.uv.u1 = 0.0f;
		back.uv.u2 = CCST_BTN_uWIDTH * scale;
		CCST_Draw2D_TexturedQuad(texturedQuadVb, &back, PACKEDCOL_WHITE);

		back.x += (short)(width / 2);
		back.uv.u1 = CCST_BTN_uWIDTH * (1.0f - scale);
		back.uv.u2 = CCST_BTN_uWIDTH;
		CCST_Draw2D_TexturedQuad(texturedQuadVb, &back, PACKEDCOL_WHITE);
	}
}

#endif

