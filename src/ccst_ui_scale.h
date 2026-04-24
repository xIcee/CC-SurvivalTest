/* Shared UI scaling helpers.
 *
 * Several modules mirror bits of Gui.c scaling logic, because some helpers are not exported
 * for dlopen'd plugins. Centralise them to reduce drift.
 */
#ifndef CCST_UI_SCALE_H
#define CCST_UI_SCALE_H

#include "Core.h"
#include "Gui.h"
#include "Graphics.h"
#include "Window.h"
#include "ExtMath.h"

static CC_INLINE float CCST_UIScale10(float value) {
	return (float)((int)(value * 10.0f + 0.5f)) / 10.0f;
}

/* Mirrors Gui.c GetWindowScale (integer). */
static CC_INLINE int CCST_UIWindowScaleInt(void) {
	float widthScale  = (float)Window_Main.Width  * Window_Main.UIScaleX;
	float heightScale = (float)Window_Main.Height * Window_Main.UIScaleY;
#ifndef CC_BUILD_DUALSCREEN
	if (!Gui_TouchUI) {
		widthScale  /= DisplayInfo.ScaleX;
		heightScale /= DisplayInfo.ScaleY;
	}
#endif
	return 1 + (int)(widthScale < heightScale ? widthScale : heightScale);
}

/* Equivalent to Gui_GetHotbarScale (linear, before DisplayInfo.ScaleX/Y). */
static CC_INLINE float CCST_UIHotbarScaleLinear(void) {
	return CCST_UIScale10((float)CCST_UIWindowScaleInt() * Gui.RawHotbarScale);
}

/* Equivalent to Gui_GetInventoryScale (linear, before sqrt). */
static CC_INLINE float CCST_UIInventoryScaleLinear(void) {
	return CCST_UIScale10((float)CCST_UIWindowScaleInt() * (Gui.RawInventoryScale * 0.5f));
}

/* TableWidget/InventoryScreen use sqrt of the linear inventory scale. */
static CC_INLINE float CCST_UIInventoryScaleRoot(void) {
	float lin = CCST_UIInventoryScaleLinear();
	if (lin < 0.01f) lin = 0.01f;
	return Math_SqrtF(lin);
}

#endif
