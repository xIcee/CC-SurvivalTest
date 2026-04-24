/*
 * Plugin-local isometric inventory icons (ClassiCube IsometricDrawer.c logic).
 * Uses CC_API Drawer_* / Gfx_DrawVb_IndexedTris_Range; does not link IsometricDrawer_*.
 */
#ifndef CCST_ISOINV_H
#define CCST_ISOINV_H
#include "BlockID.h"
struct VertexTextured;

void CCST_Iso_BeginBatch(struct VertexTextured* vertices, int* statePerQuad);
void CCST_Iso_AddBatch(struct VertexTextured** vertices, BlockID block, float size, float x, float y);
void CCST_Iso_Render(int vertexCount, int offset, int* statePerQuad);

#endif
