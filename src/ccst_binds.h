/* Shared bind-claim helper.
 *
 * ClassiCube bind mappings can be either:
 * - single button: bind->button1
 * - chorded button: hold button1 while pressing button2
 *
 * Multiple plugin modules need to check "does this key/button event correspond to a given bind?"
 * while respecting chorded binds and avoiding conflicts with other chorded binds.
 */
#ifndef CCST_BINDS_H
#define CCST_BINDS_H

#include "Input.h"

static CC_INLINE cc_bool CCST_BindMapping2_Claims(const BindMapping* bind, int btn, struct InputDevice* dev) {
	if (!bind->button2) return false;
	return dev->IsPressed(dev, bind->button1) && bind->button2 == btn;
}

/* Mirrors InputBind_Claims logic (Input.c) without referencing non-exported symbols. */
static CC_INLINE cc_bool CCST_InputBind_Claims(InputBind binding, int btn, struct InputDevice* device) {
	BindMapping* mappings = device->currentBinds;
	BindMapping* bind     = &mappings[binding];
	int i;

	if (bind->button2)
		return CCST_BindMapping2_Claims(bind, btn, device);

	for (i = 0; i < BIND_COUNT; i++) {
		if (mappings[i].button2 && CCST_BindMapping2_Claims(&mappings[i], btn, device))
			return false;
	}
	return bind->button1 == btn;
}

#endif
