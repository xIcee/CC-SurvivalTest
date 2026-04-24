#ifndef CCST_GRABSCREEN_H
#define CCST_GRABSCREEN_H

#include "Gui.h"

/* Initializes an invisible screen that:
 * - sets grabsInput so Gui.InputGrab is non-null
 * - consumes pointer and wheel events to prevent world interaction while overlay UIs are open
 *
 * The passed Screen is fully initialised (memset + VTABLE assignment). Call once per instance.
 */
void CCST_GrabScreen_InitOnce(struct Screen* s);

#endif
