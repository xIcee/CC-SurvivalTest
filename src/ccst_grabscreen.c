/* Shared invisible grab screen implementation.
 *
 * Both the survival inventory and the death overlay need a Screen that grabs input and
 * swallows pointer/wheel events. Keeping this in one file avoids duplicated VTABLE boilerplate.
 */
#include "ccst_grabscreen.h"
#include "Gui.h"
#include "Input.h"
#include "Graphics.h"
#include <string.h>
#include "ccst_binds.h"

static void CCST_Grab_Init(void* screen);
static void CCST_Grab_Update(void* screen, float delta);
static void CCST_Grab_Free(void* screen);
static void CCST_Grab_Render(void* screen, float delta);
static void CCST_Grab_BuildMesh(void* screen);
static int  CCST_Grab_InputDown(void* screen, int key, struct InputDevice* device);
static void CCST_Grab_InputUp(void* screen, int key, struct InputDevice* device);
static int  CCST_Grab_KeyPress(void* screen, char keyChar);
static int  CCST_Grab_TextChanged(void* screen, const cc_string* str);
static int  CCST_Grab_PointerDown(void* screen, int id, int x, int y);
static void CCST_Grab_PointerUp(void* screen, int id, int x, int y);
static int  CCST_Grab_PointerMove(void* screen, int id, int x, int y);
static int  CCST_Grab_MouseScroll(void* screen, float delta);
static void CCST_Grab_Layout(void* screen);
static void CCST_Grab_ContextLost(void* screen);
static void CCST_Grab_ContextRecreated(void* screen);
static int  CCST_Grab_PadAxis(void* screen, struct PadAxisUpdate* upd);

static const struct ScreenVTABLE CCST_GRAB_VTABLE = {
	CCST_Grab_Init, CCST_Grab_Update, CCST_Grab_Free,
	CCST_Grab_Render, CCST_Grab_BuildMesh,
	CCST_Grab_InputDown, CCST_Grab_InputUp, CCST_Grab_KeyPress, CCST_Grab_TextChanged,
	CCST_Grab_PointerDown, CCST_Grab_PointerUp, CCST_Grab_PointerMove, CCST_Grab_MouseScroll,
	CCST_Grab_Layout, CCST_Grab_ContextLost, CCST_Grab_ContextRecreated,
	CCST_Grab_PadAxis
};

void CCST_GrabScreen_InitOnce(struct Screen* s) {
	if (!s || s->VTABLE) return;

	memset(s, 0, sizeof(*s));
	s->VTABLE      = &CCST_GRAB_VTABLE;
	s->grabsInput  = true;
	s->blocksWorld = false;
	s->closable    = false;
}

static void CCST_Grab_Init(void* screen) {
	struct Screen* s = (struct Screen*)screen;
	s->widgets        = NULL;
	s->numWidgets     = 0;
	s->maxWidgets     = 0;
	s->widgetsPerPage = 0;
	s->selectedI      = 0;
	s->maxVertices    = 4;
}

static void CCST_Grab_Update(void* screen, float delta) { (void)screen; (void)delta; }
static void CCST_Grab_Free(void* screen) { (void)screen; }
static void CCST_Grab_Render(void* screen, float delta) { (void)screen; (void)delta; }

static void CCST_Grab_BuildMesh(void* screen) {
	struct Screen* s = (struct Screen*)screen;
	struct VertexTextured* data;
	if (!s->vb || s->maxVertices <= 0) return;
	data = (struct VertexTextured*)Gfx_LockDynamicVb(s->vb, VERTEX_FORMAT_TEXTURED, s->maxVertices);
	(void)data;
	Gfx_UnlockDynamicVb(s->vb);
}

static int CCST_Grab_InputDown(void* screen, int key, struct InputDevice* device) {
	(void)screen;

	/* While a grab screen is active (survival inventory, death overlay), prevent chat from opening. */
	if (CCST_InputBind_Claims(BIND_CHAT, key, device))      return true;
	if (CCST_InputBind_Claims(BIND_SEND_CHAT, key, device)) return true;
	if (key == CCKEY_SLASH)                            return true;
	return false;
}

static void CCST_Grab_InputUp(void* screen, int key, struct InputDevice* device) {
	(void)screen; (void)key; (void)device;
}

static int CCST_Grab_KeyPress(void* screen, char keyChar) { (void)screen; (void)keyChar; return false; }
static int CCST_Grab_TextChanged(void* screen, const cc_string* str) { (void)screen; (void)str; return false; }

static int CCST_Grab_PointerDown(void* screen, int id, int x, int y) {
	(void)screen; (void)id; (void)x; (void)y;
	return true;
}

static void CCST_Grab_PointerUp(void* screen, int id, int x, int y) {
	(void)screen; (void)id; (void)x; (void)y;
}

static int CCST_Grab_PointerMove(void* screen, int id, int x, int y) {
	(void)screen; (void)id; (void)x; (void)y;
	return true;
}

static int CCST_Grab_MouseScroll(void* screen, float delta) { (void)screen; (void)delta; return true; }
static void CCST_Grab_Layout(void* screen) { (void)screen; }

static void CCST_Grab_ContextLost(void* screen) {
	struct Screen* s = (struct Screen*)screen;
	Gfx_DeleteDynamicVb(&s->vb);
	s->vb = 0;
}

static void CCST_Grab_ContextRecreated(void* screen) {
	struct Screen* s = (struct Screen*)screen;
	Gfx_DeleteDynamicVb(&s->vb);
	s->vb = Gfx_CreateDynamicVb(VERTEX_FORMAT_TEXTURED, s->maxVertices);
}

static int CCST_Grab_PadAxis(void* screen, struct PadAxisUpdate* upd) { (void)screen; (void)upd; return false; }

