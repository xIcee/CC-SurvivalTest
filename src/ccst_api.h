/* Helpers using only CC_API / CC_VAR ClassiCube symbols (see ClassiCube/doc/plugin-dev.md).
 * netshim.c replaces Protocol.Handlers[OPCODE_VELOCITY_CONTROL] (CC_VAR) like ref/hax.c, audit nm -u accordingly.
 * peer.c wraps Server.SendData (CC_VAR) and Protocol.Handlers for position/orientation + TWO_WAY_PING for P2P melee.
 * Ping_AveragePingMS / Ping_Update are not CC_API, peer mirrors CPE two-way ping RTT with Stopwatch (see peer.c).
 * Camera shake: CCST_Health_OnFrame(delta) from Draw2D (Survival Test b.d.b sin envelope + hurtDir).
 * Avoid: Chat_AddRaw, Gfx_Draw2DFlat (use Gfx_CreateDynamicVb + VertexColoured + DrawVb_IndexedTris_Range),
 * Gfx_Draw2DTexture, Gfx_Make2DQuad, IsometricDrawer_* (survival inventory uses isoinv.c + CC_API Drawer_*),
 * InventoryScreen_Hide (use Gui_GetScreen(GUI_PRIORITY_INVENTORY) + Gui_Remove),
 * fakeinventory uses Gui_Add(..., GUI_PRIORITY_INVENTORY) with an invisible grabsInput Screen so
 * Gui.InputGrab is set (Camera_CheckFocus + no per-frame cursor warp); do not rely on manual LoseFocus alone,
 * Gui_GetHotbarScale (not CC_API; mirror Gui.c / _GraphicsBase.h),
 * HacksComp_Set*, Entity_TouchesAnyWater, Math_Ceil,
 * LocalPlayers_MoveToSpawn, Game_HideGui, Game_ClassicMode, InputBind_Claims (mirror Input.c; uses device->currentBinds),
 * plain `extern` globals not marked CC_VAR (e.g. `Input` / Input.DownHook; peer.c mirrors LMB via InputEvents.Down2/Up2),
 * `Game_Username` (deathmsg.c: TabList self id + Options_Get LOPT_USERNAME),
 * `Game_SelectedPos` (use `Camera.Active->GetPickedBlock` into a local `struct RayTracer`),
 * `AutoRotate_Enabled` (not CC_VAR; avoid or mirror logic without importing),
 * `Entity_GetBounds` (use `AABB_Make(bb, &e->Position, &e->Size)` like Entity.c),
 * `IVec3_ToVec3` (not CC_API; assign `(float)a->x` etc.),
 * `Game_CanPick` / `Game_BreakableLiquids` (mirror Game.c; liquids use `Options_GetBool(OPT_MODIFIABLE_LIQUIDS, false)`),
 * `Gui_GetBlocksWorld` (iterate `Gui.Screens[0..ScreensCount)` for `blocksWorld`),
 * `AABB_Intersects` (inline Physics.c overlap test).
 * `Intersection_RayIntersectsBox` / `Intersection_RayIntersectsRotatedBox` are not CC_API, peer.c inlines slab tests; melee uses `AABB_Make` + collision `Size`.
 */
#ifndef CCST_API_H
#define CCST_API_H
#include "Chat.h"
#include "String_.h"

/* Chat_AddRaw is declared in Chat.h but not CC_API, mirror Chat.c. */
static CC_INLINE void CCST_Chat(const char* raw) {
	cc_string str = String_FromReadonly(raw);
	Chat_Add(&str);
}

#endif
