/*
    deathscreen.c
    survival game over overlay.

    - score display and respawn button
    - input grabber while active
*/
#include "PluginAPI.h"
#include "Drawer2D.h"
#include "Event.h"
#include "ExtMath.h"
#include "Funcs.h"
#include "Game.h"
#include "Graphics.h"
#include "Gui.h"
#include "Input.h"
#include "PackedCol.h"
#include "String_.h"
#include "Window.h"
#include <string.h>
#include "deathscreen.h"
#include "fakeinventory.h"
#include "health.h"
#include "respawn.h"
#include "score.h"
#include "ccst_binds.h"
#include "ccst_ui_scale.h"
#include "ccst_font.h"
#include "ccst_draw2d.h"
#include "ccst_drawtext.h"
#include "ccst_button.h"
#include "ccst_util.h"

static cc_bool ccst_death_active;
static int ccst_death_final_score;

static GfxResourceID ccst_death_flatVb;
static GfxResourceID ccst_death_texVb;
static struct FontDesc ccst_death_fontTitle;
static struct FontDesc ccst_death_fontSub;
static cc_bool ccst_death_fontsReady;
static int ccst_death_fontTitlePt = -1;
static int ccst_death_fontSubPt   = -1;

/* Match classic MC GUI button sizing (in 96-DPI GUI units). */
#define CCST_DEATH_BTN_W 200
#define CCST_DEATH_BTN_H 20

/* Survival Test game-over screen background gradient colors (ARGB in Java). */
#define CCST_ST_BG_TOP PackedCol_Make(80,  0,  0,  96)  /* 0x60500000 */
#define CCST_ST_BG_BOT PackedCol_Make(128, 48, 48, 160) /* 0xA0803030 */

static int ccst_death_hover_btn; /* 0 none, 1 respawn, 2 title */
static cc_bool ccst_death_grab_added;

/* Invisible screen: grabsInput and consumes pointer/wheel like fakeinventory. */
static struct Screen ccst_deathGrabScreen;

static float CCST_Death_MenuRootScale(void) {
	return CCST_UIInventoryScaleRoot();
}

static void CCST_Death_Draw2DFlat(int x, int y, int width, int height, PackedCol color) {
	CCST_Draw2D_Flat(&ccst_death_flatVb, x, y, width, height, color);
}

static void CCST_Death_Draw2DGradient(int x, int y, int width, int height, PackedCol top, PackedCol bottom) {
	CCST_Draw2D_Gradient(&ccst_death_flatVb, x, y, width, height, top, bottom);
}

static void CCST_Death_DrawButtonBackground(int x, int y, int width, int height, cc_bool selected, cc_bool disabled) {
	/* Fallback for rare cases (GUI texture not ready). */
	if (!(Gui.ClassicTexture ? Gui.GuiClassicTex : Gui.GuiTex)) {
		CCST_Death_Draw2DFlat(x, y, width, height, PackedCol_Make(40, 40, 40, 220));
		return;
	}
	CCST_DrawButtonBackground(&ccst_death_texVb, x, y, width, height, selected, disabled);
}

static void CCST_Death_EnsureFonts(void) {
	if (ccst_death_fontsReady) return;
	/* Match ClassiCube menu typography (Gui_MakeTitleFont/BodyFont in Gui.c). */
	CCST_Font_EnsurePoint(&ccst_death_fontTitle, &ccst_death_fontTitlePt, 16, 16, 16, FONT_FLAGS_BOLD);
	CCST_Font_EnsurePoint(&ccst_death_fontSub,   &ccst_death_fontSubPt,   16, 16, 16, FONT_FLAGS_NONE);
	ccst_death_fontsReady = true;
}

static void CCST_Death_FreeFonts(void) {
	if (!ccst_death_fontsReady) return;
	CCST_Font_FreeCached(&ccst_death_fontTitle, &ccst_death_fontTitlePt);
	CCST_Font_FreeCached(&ccst_death_fontSub,   &ccst_death_fontSubPt);
	ccst_death_fontsReady = false;
}

static void CCST_Death_CompleteRespawn(void) {
	if (!ccst_death_active) return;
	if (!CCST_Health_CanRespawn()) return;
	ccst_death_active = false;
	if (ccst_death_grab_added) {
		Gui_Remove(&ccst_deathGrabScreen);
		ccst_death_grab_added = false;
	}
	CCST_Respawn_Player();
	CCST_Health_RespawnFromDeath();
}

static cc_bool CCST_Death_HitRect(int mx, int my, int x, int y, int w, int h) {
	return mx >= x && mx < x + w && my >= y && my < y + h;
}

static void CCST_Death_UpdateHoverAt(int mx, int my) {
	int bx, by;
	int bw, bh;
	float root;

	ccst_death_hover_btn = 0;
	if (!ccst_death_active) return;

	root = CCST_Death_MenuRootScale();
	bw = Display_ScaleX((int)(CCST_DEATH_BTN_W * root + 0.5f));
	/* Match ButtonWidget_Init: minHeight = Display_ScaleY(40). */
	bh = Display_ScaleY(40);
	bx = Game.Width / 2 - bw / 2;
	by = Game.Height / 4 + Display_ScaleY((int)(72.0f * root + 0.5f));

	if (CCST_Death_HitRect(mx, my, bx, by, bw, bh)) {
		ccst_death_hover_btn = 1;
		return;
	}
	if (CCST_Death_HitRect(mx, my, bx, by + Display_ScaleY((int)(24.0f * root + 0.5f)), bw, bh)) {
		ccst_death_hover_btn = 2;
		return;
	}
}

static void CCST_Death_UpdateHover(void) {
	CCST_Death_UpdateHoverAt(Pointers[0].x, Pointers[0].y);
}

static void CCST_DeathGrab_Init(void* screen);
static void CCST_DeathGrab_Update(void* screen, float delta);
static void CCST_DeathGrab_Free(void* screen);
static void CCST_DeathGrab_Render(void* screen, float delta);
static void CCST_DeathGrab_BuildMesh(void* screen);
static int  CCST_DeathGrab_InputDown(void* screen, int key, struct InputDevice* device);
static void CCST_DeathGrab_InputUp(void* screen, int key, struct InputDevice* device);
static int  CCST_DeathGrab_KeyPress(void* screen, char keyChar);
static int  CCST_DeathGrab_TextChanged(void* screen, const cc_string* str);
static int  CCST_DeathGrab_PointerDown(void* screen, int id, int x, int y);
static void CCST_DeathGrab_PointerUp(void* screen, int id, int x, int y);
static int  CCST_DeathGrab_PointerMove(void* screen, int id, int x, int y);
static int  CCST_DeathGrab_MouseScroll(void* screen, float delta);
static void CCST_DeathGrab_Layout(void* screen);
static void CCST_DeathGrab_ContextLost(void* screen);
static void CCST_DeathGrab_ContextRecreated(void* screen);
static int  CCST_DeathGrab_PadAxis(void* screen, struct PadAxisUpdate* upd);

static const struct ScreenVTABLE ccst_deathGrab_VTABLE = {
	CCST_DeathGrab_Init, CCST_DeathGrab_Update, CCST_DeathGrab_Free,
	CCST_DeathGrab_Render, CCST_DeathGrab_BuildMesh,
	CCST_DeathGrab_InputDown, CCST_DeathGrab_InputUp, CCST_DeathGrab_KeyPress, CCST_DeathGrab_TextChanged,
	CCST_DeathGrab_PointerDown, CCST_DeathGrab_PointerUp, CCST_DeathGrab_PointerMove, CCST_DeathGrab_MouseScroll,
	CCST_DeathGrab_Layout, CCST_DeathGrab_ContextLost, CCST_DeathGrab_ContextRecreated,
	CCST_DeathGrab_PadAxis
};

static void CCST_DeathGrab_InitOnce(void) {
	struct Screen* s = &ccst_deathGrabScreen;
	if (s->VTABLE) return;

	memset(s, 0, sizeof(*s));
	s->VTABLE      = &ccst_deathGrab_VTABLE;
	s->grabsInput  = true;
	s->blocksWorld = false;
	s->closable    = false;
}

static void CCST_DeathGrab_Init(void* screen) {
	struct Screen* s = (struct Screen*)screen;
	s->widgets        = NULL;
	s->numWidgets     = 0;
	s->maxWidgets     = 0;
	s->widgetsPerPage = 0;
	s->selectedI      = 0;
	s->maxVertices    = 4;
}
static void CCST_DeathGrab_Update(void* screen, float delta) { (void)screen; (void)delta; }
static void CCST_DeathGrab_Free(void* screen) { (void)screen; }
static void CCST_DeathGrab_Render(void* screen, float delta) { (void)screen; (void)delta; }
static void CCST_DeathGrab_BuildMesh(void* screen) {
	struct Screen* s = (struct Screen*)screen;
	struct VertexTextured* data;
	if (!s->vb || s->maxVertices <= 0) return;
	data = (struct VertexTextured*)Gfx_LockDynamicVb(s->vb, VERTEX_FORMAT_TEXTURED, s->maxVertices);
	(void)data;
	Gfx_UnlockDynamicVb(s->vb);
}

static int CCST_DeathGrab_InputDown(void* screen, int key, struct InputDevice* device) {
	(void)screen;
	if (!ccst_death_active) return false;
	if (key == CCKEY_ENTER || CCST_InputBind_Claims(BIND_RESPAWN, key, device)) {
		if (CCST_Health_CanRespawn())
			CCST_Death_CompleteRespawn();
		return true;
	}
	return false;
}
static void CCST_DeathGrab_InputUp(void* screen, int key, struct InputDevice* device) { (void)screen; (void)key; (void)device; }
static int CCST_DeathGrab_KeyPress(void* screen, char keyChar) { (void)screen; (void)keyChar; return false; }
static int CCST_DeathGrab_TextChanged(void* screen, const cc_string* str) { (void)screen; (void)str; return false; }

static int CCST_DeathGrab_PointerDown(void* screen, int id, int x, int y) {
	CCST_UNUSED(screen);
	CCST_UNUSED(id);
	if (!ccst_death_active) return false;
	/* Click buttons, not anywhere. */
	CCST_Death_UpdateHoverAt(x, y);
	if (ccst_death_hover_btn == 1 && CCST_Health_CanRespawn()) CCST_Death_CompleteRespawn();
	return true;
}
static void CCST_DeathGrab_PointerUp(void* screen, int id, int x, int y) { (void)screen; (void)id; (void)x; (void)y; }
static int CCST_DeathGrab_PointerMove(void* screen, int id, int x, int y) {
	CCST_UNUSED(screen);
	CCST_UNUSED(id);
	if (!ccst_death_active) return false;
	CCST_Death_UpdateHoverAt(x, y);
	return true;
}
static int CCST_DeathGrab_MouseScroll(void* screen, float delta) { (void)screen; (void)delta; return ccst_death_active; }
static void CCST_DeathGrab_Layout(void* screen) { (void)screen; }
static void CCST_DeathGrab_ContextLost(void* screen) {
	struct Screen* s = (struct Screen*)screen;
	Gfx_DeleteDynamicVb(&s->vb);
	s->vb = 0;
}
static void CCST_DeathGrab_ContextRecreated(void* screen) {
	struct Screen* s = (struct Screen*)screen;
	Gfx_DeleteDynamicVb(&s->vb);
	s->vb = Gfx_CreateDynamicVb(VERTEX_FORMAT_TEXTURED, s->maxVertices);
}
static int CCST_DeathGrab_PadAxis(void* screen, struct PadAxisUpdate* upd) { (void)screen; (void)upd; return false; }

void CCST_Deathscreen_Init(void) {
	ccst_death_active = false;
	ccst_death_final_score = 0;
	ccst_death_flatVb = 0;
	ccst_death_texVb = 0;
	ccst_death_fontsReady = false;
	ccst_death_fontTitlePt = -1;
	ccst_death_fontSubPt   = -1;
	ccst_death_hover_btn = 0;
	ccst_death_grab_added = false;
	CCST_DeathGrab_InitOnce();
}

void CCST_Deathscreen_Free(void) {
	if (ccst_death_grab_added) {
		Gui_Remove(&ccst_deathGrabScreen);
		ccst_death_grab_added = false;
	}
	CCST_Death_FreeFonts();
	Gfx_DeleteDynamicVb(&ccst_death_flatVb);
	Gfx_DeleteDynamicVb(&ccst_death_texVb);
	ccst_death_active = false;
}

cc_bool CCST_Deathscreen_IsActive(void) {
	return ccst_death_active;
}

void CCST_Deathscreen_Begin(int finalScore) {
	ccst_death_final_score = finalScore;
	CCST_Score_Reset();
	CCST_Inv_ClearAll();
	ccst_death_active = true;
	ccst_death_hover_btn = 0;
	CCST_DeathGrab_InitOnce();
	/* Grab input above most screens, but keep it cosmetic (blocksWorld=false). */
	Gui_Add(&ccst_deathGrabScreen, GUI_PRIORITY_MENU);
	ccst_death_grab_added = true;
}

void CCST_Deathscreen_ForceCancel(void) {
	ccst_death_active = false;
	if (ccst_death_grab_added) {
		Gui_Remove(&ccst_deathGrabScreen);
		ccst_death_grab_added = false;
	}
}

void CCST_Deathscreen_Draw2D(float delta) {
	cc_string line;
	char buf[STRING_SIZE];
	struct DrawTextArgs args;
	int y;
	int bx, by, bw, bh;

	(void)delta;
	if (!ccst_death_active || Gfx.LostContext) return;

	CCST_Death_Draw2DGradient(0, 0, Game.Width, Game.Height, CCST_ST_BG_TOP, CCST_ST_BG_BOT);

	CCST_Death_EnsureFonts();

	/* Reference draws title at y=30 with GL scale 2 -> ~60px. */
	{
		float root = CCST_Death_MenuRootScale();
		y = Display_ScaleY((int)(60.0f * root + 0.5f));
	}
	String_InitArray(line, buf);
	String_AppendConst(&line, "&fGame over!");
	args.text = line;
	args.font = &ccst_death_fontTitle;
	args.useShadow = true;
	/* Title is rendered at 2x scale in the reference client. */
	{
		int th = 0;
		CCST_Draw2D_TextScaledCenteredX(&ccst_death_texVb, &args, Game.Width / 2, y, 2, PACKEDCOL_WHITE, &th);
		if (th > 0)
			y += th + (int)((float)Window_Main.Height * 0.02f + 0.5f);
		else
			y += (int)((float)Window_Main.Height * 0.06f + 0.5f);
	}

	line.length = 0;
	/* Reference string: "Score: &e<n>" at y=100. */
	String_AppendConst(&line, "&fScore: &e");
	String_AppendInt(&line, ccst_death_final_score);
	args.text = line;
	args.font = &ccst_death_fontSub;
	{
		float root = CCST_Death_MenuRootScale();
		int sy = Display_ScaleY((int)(100.0f * root + 0.5f));
		CCST_Draw2D_TextCenteredX(&ccst_death_texVb, &args, Game.Width / 2, sy, PACKEDCOL_WHITE);
	}

	/* Buttons */
	{
		float root = CCST_Death_MenuRootScale();
		bw = Display_ScaleX((int)(CCST_DEATH_BTN_W * root + 0.5f));
		bh = Display_ScaleY(40);
		by = Game.Height / 4 + Display_ScaleY((int)(72.0f * root + 0.5f));
	}
	bx = Game.Width / 2 - bw / 2;
	CCST_Death_UpdateHover();

	/* Button backgrounds (use gui.png/gui_classic.png like ButtonWidget) */
	{
		cc_bool respawnDisabled = !CCST_Health_CanRespawn();
		CCST_Death_DrawButtonBackground(bx, by, bw, bh, ccst_death_hover_btn == 1 && !respawnDisabled, respawnDisabled);
	}
	{
		float root = CCST_Death_MenuRootScale();
		CCST_Death_DrawButtonBackground(bx, by + Display_ScaleY((int)(24.0f * root + 0.5f)), bw, bh, ccst_death_hover_btn == 2, true);
	}

	/* Button labels */
	args.font = &ccst_death_fontSub;
	args.useShadow = true;
	{
		PackedCol normColor     = PackedCol_Make(224, 224, 224, 255);
		PackedCol activeColor   = PackedCol_Make(255, 255, 160, 255);
		PackedCol disabledColor = PackedCol_Make(160, 160, 160, 255);
		cc_bool respawnDisabled = !CCST_Health_CanRespawn();
		PackedCol respawnCol    = respawnDisabled ? disabledColor : ((ccst_death_hover_btn == 1) ? activeColor : normColor);
		PackedCol titleCol      = disabledColor;

		line.length = 0;
		String_AppendConst(&line, "Respawn");
		args.text = line;
		CCST_Draw2D_TextCenteredInRect(&ccst_death_texVb, &args, bx, by, bw, bh, respawnCol);

		line.length = 0;
		String_AppendConst(&line, "Title menu");
		args.text = line;
		{
			float root = CCST_Death_MenuRootScale();
			int by2 = by + Display_ScaleY((int)(24.0f * root + 0.5f));
			CCST_Draw2D_TextCenteredInRect(&ccst_death_texVb, &args, bx, by2, bw, bh, titleCol);
		}
	}
}
