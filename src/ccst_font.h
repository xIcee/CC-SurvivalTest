/* Shared font lifecycle helpers for plugin UIs.
 *
 * Many plugin modules need "ensure font at point size X; if changed, free+recreate" while
 * avoiding leaks and keeping point-size clamping consistent.
 */
#ifndef CCST_FONT_H
#define CCST_FONT_H

#include "Core.h"
#include "Drawer2D.h"

/* Ensures `font` is created for `point` and `flags`.
 * - `cached_point` should be initialised to -1 and is updated on success.
 * - If point is unchanged, this is a no-op.
 * - If point changes, the previous font (if any) is freed then recreated.
 */
static CC_INLINE void CCST_Font_EnsurePoint(
	struct FontDesc* font, int* cached_point,
	int point, int min_point, int max_point, int flags
) {
	if (!font || !cached_point) return;
	if (point < min_point) point = min_point;
	if (point > max_point) point = max_point;
	if (point == *cached_point) return;

	if (*cached_point >= 0) Font_Free(font);
	Font_Make(font, point, flags);
	*cached_point = point;
}

static CC_INLINE void CCST_Font_FreeCached(struct FontDesc* font, int* cached_point) {
	if (!font || !cached_point) return;
	if (*cached_point >= 0) {
		Font_Free(font);
		*cached_point = -1;
	}
}

#endif
