/*
    blockbreak.c
    Survival-style progressive block mining.

    - manages mining duration and hardness
    - mining overlay and visual progress
    - sound effects via ground-spoofing
    - post-break delay logic
*/

#include "PluginAPI.h"
#include "Block.h"
#include "BlockID.h"
#include "Constants.h"
#include "Event.h"
#include "Camera.h"
#include "Game.h"
#include "Gui.h"
#include "InputHandler.h"
#include "Entity.h"
#include "Picking.h"
#include "SelectionBox.h"
#include "World.h"
#include "health.h"
#include "policy.h"
#include "blockbreak.h"
#include "blocktype.h"
#include "fakeinventory.h"
#include "deathscreen.h"
#include <math.h>

#define CCST_BREAK_POST_TICKS 5
#define CCST_BREAK_POST_DELAY_SEC ((float)CCST_BREAK_POST_TICKS * (1.0f / 20.0f))

#define CCST_BREAK_OVERLAY_SEL_ID ((cc_uint8)255)
#define CCST_BREAK_UNMINEABLE_ALPHA ((cc_uint8)0)

static void CCST_BreakOverlay_Remove(void) {
	Selections_Remove(CCST_BREAK_OVERLAY_SEL_ID);
}

static cc_bool ccst_blockbreak_running;
static cc_bool ccst_pending_ground_flash;
static BlockID ccst_sound_mine_block;

static cc_bool ccst_sound_restore_pending;
static BlockID ccst_sound_feet_block;
static cc_uint8 ccst_sound_old_step;
static Vec3 ccst_sound_saved_vel;

// check block under player for sound effects
static BlockID CCST_FeetBlockUnderPlayer(const struct LocalPlayer* p) {
	Vec3 pos;
	IVec3 coords;
	BlockID blockUnder;
	float maxY;

	pos = p->Base.Position;
	pos.y -= 0.01f;
	coords.x = (int)floorf(pos.x);
	coords.y = (int)floorf(pos.y);
	coords.z = (int)floorf(pos.z);
	if (!World_Contains(coords.x, coords.y, coords.z)) return BLOCK_AIR;
	blockUnder = World_GetBlock(coords.x, coords.y, coords.z);
	maxY = (float)coords.y + Blocks.MaxBB[blockUnder].y;
	if (maxY >= pos.y && Blocks.Collide[blockUnder] == COLLIDE_SOLID)
		return blockUnder;
	return BLOCK_AIR;
}

static void CCST_BlockBreak_FinishSoundRestore(void) {
	struct LocalPlayer* p;

	if (!ccst_sound_restore_pending) return;
	p = Entities.CurPlayer;
	if (p)
		p->Base.Velocity = ccst_sound_saved_vel;
	if (ccst_sound_feet_block < BLOCK_COUNT)
		Blocks.StepSounds[ccst_sound_feet_block] = ccst_sound_old_step;
	ccst_sound_restore_pending = false;
}

static void CCST_BlockBreak_PreEntityTick(struct ScheduledTask* task) {
	struct LocalPlayer* p;
	BlockID feet;
	(void)task;

	/* Draw2D should clear this; recover if a frame skipped the hook. */
	if (ccst_sound_restore_pending)
		CCST_BlockBreak_FinishSoundRestore();

	if (!ccst_blockbreak_running || !ccst_pending_ground_flash) return;
	if (CCST_Health_IsDeadOrDying()) return;
	p = Entities.CurPlayer;
	if (!p || !World.Loaded) return;
	if (p->Hacks.Noclip || p->Hacks.Flying) return;
	if (!p->Base.OnGround) return;

	feet = CCST_FeetBlockUnderPlayer(p);
	if (feet >= BLOCK_COUNT || ccst_sound_mine_block >= BLOCK_COUNT) {
		ccst_pending_ground_flash = false;
		return;
	}
	if (feet == BLOCK_AIR || Blocks.Draw[feet] == DRAW_GAS) {
		ccst_pending_ground_flash = false;
		return;
	}

	ccst_sound_feet_block   = feet;
	ccst_sound_old_step     = Blocks.StepSounds[feet];
	Blocks.StepSounds[feet] = Blocks.StepSounds[ccst_sound_mine_block];

	ccst_sound_saved_vel = p->Base.Velocity;
	p->Base.OnGround     = false;

	ccst_sound_restore_pending = true;
	ccst_pending_ground_flash  = false;
}

static void CCST_RaiseUserBlockChanged(IVec3 coords, BlockID oldBlock, BlockID block) {
	int i;
	struct Event_Block* h = &UserEvents.BlockChanged;
	for (i = 0; i < h->Count; i++) {
		h->Handlers[i](h->Objs[i], coords, oldBlock, block);
	}
}

static cc_bool ccst_shadow_may_delete[BLOCK_COUNT];
static cc_bool ccst_patched_can_delete;
static cc_bool ccst_prev_survival_break;

static IVec3 ccst_brk_pos;
static BlockID ccst_brk_block;
static float ccst_brk_accum;
static float ccst_brk_need;
static float ccst_brk_dig_cd;
static cc_bool ccst_brk_active;
static float ccst_brk_after_cooldown; // seconds; after a break completes

/* whether this client is currently permitted to delete this block.
   NOTE: In survival mode we patch Blocks.CanDelete[] to false to stop vanilla instant-break.
   therefore we must consult our shadow copy of server permissions here. */
static cc_bool CCST_BlockBreak_CanDelete(BlockID b) {
	if (b == BLOCK_AIR || b >= BLOCK_COUNT) return false;
	if (Blocks.Draw[b] == DRAW_GAS) return false;
	return ccst_shadow_may_delete[b];
}

static void CCST_BlockBreak_ResetDig(void) {
	ccst_brk_active = false;
	ccst_brk_accum  = 0.0f;
	ccst_brk_need   = 0.0f;
	ccst_brk_dig_cd = 0.0f;
	ccst_brk_block  = BLOCK_AIR;
	ccst_pending_ground_flash = false;
	CCST_BreakOverlay_Remove();
}

static void CCST_UnpatchCanDelete(void) {
	int i;
	if (!ccst_patched_can_delete) return;
	for (i = 0; i < BLOCK_COUNT; i++)
		Blocks.CanDelete[i] = ccst_shadow_may_delete[i];
	ccst_patched_can_delete = false;
}

static cc_bool CCST_BlockBreak_SurvivalActive(void) {
	return CCST_Policy_PluginEnabled() && CCST_Policy_MiningEnabled() && World.Loaded;
}

// apply the "no instant delete" patch based on current shadow perms.
static void CCST_ApplyDeletePatchFromShadow(void) {
	int i;
	if (!CCST_BlockBreak_SurvivalActive()) return;

	for (i = 0; i < BLOCK_COUNT; i++) {
		if (!ccst_shadow_may_delete[i]) continue;
		if (Blocks.Draw[i] == DRAW_GAS) continue;
		Blocks.CanDelete[i] = false;
	}
	ccst_patched_can_delete = true;
}

/* full resync: only safe when we want to trust Blocks.CanDelete as server truth
   (i.e. when we are not trying to preserve a just-received single-block update). */
static void CCST_ResyncShadowFromBlocks(void) {
	int i;

	CCST_UnpatchCanDelete();
	for (i = 0; i < BLOCK_COUNT; i++)
		ccst_shadow_may_delete[i] = Blocks.CanDelete[i];

	if (!CCST_BlockBreak_SurvivalActive()) {
		ccst_brk_after_cooldown = 0.0f;
		CCST_BlockBreak_ResetDig();
		return;
	}

	CCST_ApplyDeletePatchFromShadow();
}

void CCST_BlockBreak_OnSurvivalStateMayHaveChanged(void) {
	CCST_ResyncShadowFromBlocks();
}

static void CCST_OnBlockOrPermChanged(void* obj) {
	(void)obj;
	/* permissionsChanged does not identify *which* block changed, so we cannot safely
	   resync from Blocks.CanDelete while patched.
	   netshim updates ccst_shadow_may_delete per-packet so here we just re-apply patch
	   (e.g. if a block becomes DRAW_GAS/non-gas, or if some other system wrote CanDelete). */
	CCST_UnpatchCanDelete();
	if (!CCST_BlockBreak_SurvivalActive()) return;
	CCST_ApplyDeletePatchFromShadow();
}

void CCST_BlockBreak_OnMapLoaded(void) {
	ccst_brk_after_cooldown = 0.0f;
	CCST_BlockBreak_ResetDig();
	CCST_ResyncShadowFromBlocks();
	ccst_prev_survival_break = CCST_BlockBreak_SurvivalActive();
}

void CCST_BlockBreak_Init(void) {
	ccst_blockbreak_running = true;
	ccst_patched_can_delete = false;
	ccst_prev_survival_break = false;
	CCST_BlockBreak_ResetDig();
	ScheduledTask_Add(GAME_DEF_TICKS, CCST_BlockBreak_PreEntityTick);
	Event_Register_(&BlockEvents.PermissionsChanged, NULL, CCST_OnBlockOrPermChanged);
	Event_Register_(&BlockEvents.BlockDefChanged, NULL, CCST_OnBlockOrPermChanged);
}

void CCST_BlockBreak_Free(void) {
	ccst_blockbreak_running = false;
	ccst_pending_ground_flash = false;
	ccst_brk_after_cooldown = 0.0f;
	CCST_BlockBreak_FinishSoundRestore();
	Event_Unregister_(&BlockEvents.PermissionsChanged, NULL, CCST_OnBlockOrPermChanged);
	Event_Unregister_(&BlockEvents.BlockDefChanged, NULL, CCST_OnBlockOrPermChanged);
	CCST_UnpatchCanDelete();
	CCST_BlockBreak_ResetDig();
}

void CCST_BlockBreak_OnSetBlockPermission(BlockID block, cc_bool canDelete) {
	if (block >= BLOCK_COUNT) return;
	ccst_shadow_may_delete[block] = canDelete;

	// keep the live patch coherent even if this packet arrives mid-game.
	if (!ccst_patched_can_delete) return;
	if (!CCST_BlockBreak_SurvivalActive()) return;
	if (Blocks.Draw[block] == DRAW_GAS) return;
	if (canDelete) Blocks.CanDelete[block] = false;
}

void CCST_BlockBreak_Draw2D(float delta) {
	struct RayTracer pick;
	IVec3 pos;
	BlockID b;
	cc_bool surv, left;
	float tneed;

	CCST_BlockBreak_FinishSoundRestore();

	if (delta > 0.1f) delta = 0.1f;
	if (delta <= 0.0f) {
		CCST_Inv_Draw2D(delta);
		CCST_Deathscreen_Draw2D(delta);
		CCST_Health_ApplyZoomForNextFrame();
		return;
	}

	surv = CCST_BlockBreak_SurvivalActive();
	if (surv != ccst_prev_survival_break) {
		ccst_prev_survival_break = surv;
		CCST_ResyncShadowFromBlocks();
	}

	if (!World.Loaded || !Entities.CurPlayer) {
		ccst_brk_after_cooldown = 0.0f;
		CCST_BlockBreak_ResetDig();
		CCST_Inv_Draw2D(delta);
		CCST_Deathscreen_Draw2D(delta);
		CCST_Health_ApplyZoomForNextFrame();
		return;
	}

	if (!surv) {
		ccst_brk_after_cooldown = 0.0f;
		CCST_BlockBreak_ResetDig();
		CCST_Inv_Draw2D(delta);
		CCST_Deathscreen_Draw2D(delta);
		CCST_Health_ApplyZoomForNextFrame();
		return;
	}

	if (CCST_Health_IsDeadOrDying()) {
		CCST_BlockBreak_ResetDig();
		CCST_Inv_Draw2D(delta);
		CCST_Deathscreen_Draw2D(delta);
		CCST_Health_ApplyZoomForNextFrame();
		return;
	}

	if (ccst_brk_after_cooldown > 0.0f) {
		ccst_brk_after_cooldown -= delta;
		if (ccst_brk_after_cooldown < 0.0f)
			ccst_brk_after_cooldown = 0.0f;
	}

	if (CCST_Inv_IsOpen()) {
		CCST_BlockBreak_ResetDig();
	} else if (Gui.InputGrab) {
		CCST_BlockBreak_ResetDig();
	} else {
		left = KeyBind_IsPressed(BIND_DELETE_BLOCK);
		if (!left) {
			CCST_BlockBreak_ResetDig();
		} else if (ccst_brk_after_cooldown > 0.0f) {
			// post-break cooldown; no mining this frame
		} else if (!Camera.Active || !Camera.Active->GetPickedBlock) {
			CCST_BlockBreak_ResetDig();
		} else {
			Camera.Active->GetPickedBlock(&pick);
			if (!pick.valid || !World_Contains(pick.pos.x, pick.pos.y, pick.pos.z)) {
				CCST_BlockBreak_ResetDig();
			} else {
				pos = pick.pos;
				b   = World_GetBlock(pos.x, pos.y, pos.z);
				if (Blocks.Draw[b] == DRAW_GAS) {
					CCST_BlockBreak_ResetDig();
				} else {
					if (CCST_BlockBreak_CanDelete(b))
						tneed = CCST_BlockType_BreakTimeFor(b);
					else
						// Not permitted to delete: infinite "time"
						tneed = INFINITY;

					if (!ccst_brk_active || ccst_brk_pos.x != pos.x || ccst_brk_pos.y != pos.y || ccst_brk_pos.z != pos.z || ccst_brk_block != b) {
						ccst_brk_pos    = pos;
						ccst_brk_block  = b;
						ccst_brk_need   = tneed;
						ccst_brk_accum  = 0.0f;
						ccst_brk_active = true;
						ccst_brk_dig_cd = 0.22f;
						ccst_sound_mine_block     = b;
						ccst_pending_ground_flash = true;
					}

					ccst_brk_accum += delta;
					ccst_brk_dig_cd -= delta;
					{
						int bursts = 0;
						while (ccst_brk_dig_cd <= 0.0f && ccst_brk_active && bursts < 6) {
							ccst_sound_mine_block     = b;
							ccst_pending_ground_flash = true;
							ccst_brk_dig_cd += 0.175f;
							bursts++;
						}
					}

					if (ccst_brk_active && ccst_brk_need > 0.0f) {
						IVec3 p1, p2;
						cc_uint8 a;

						if (isinf((double)ccst_brk_need)) {
							a = CCST_BREAK_UNMINEABLE_ALPHA;
						} else {
							float u, t;
							u = ccst_brk_accum / ccst_brk_need;
							if (u > 1.0f) u = 1.0f;
							t = u * u; // ease-in quad
							a = (cc_uint8)(t * (255.0f * 0.5f) + 0.5f); // max alpha 127 (~50%)
						}
						p1.x = pos.x;     p1.y = pos.y;     p1.z = pos.z;
						p2.x = pos.x + 1; p2.y = pos.y + 1; p2.z = pos.z + 1;
						Selections_Add(CCST_BREAK_OVERLAY_SEL_ID, &p1, &p2, PackedCol_Make(255, 255, 255, a));
					}

					if (!isinf((double)ccst_brk_need) && ccst_brk_accum >= ccst_brk_need) {
						BlockID old = World_GetBlock(pos.x, pos.y, pos.z);
						if (old == b && Blocks.Draw[old] != DRAW_GAS) {
							Game_ChangeBlock(pos.x, pos.y, pos.z, BLOCK_AIR);
							CCST_RaiseUserBlockChanged(pos, old, BLOCK_AIR);
							CCST_Inv_TryPickupMinedBlock(old);
						}
						CCST_BlockBreak_ResetDig();
						ccst_brk_after_cooldown = CCST_BREAK_POST_DELAY_SEC;
					}
				}
			}
		}
	}

	CCST_Inv_Draw2D(delta);
	CCST_Deathscreen_Draw2D(delta);
	/* apply zoom for next 3D frame without breaking UI
	   (doing this at any other time will break our ui) */
	CCST_Health_ApplyZoomForNextFrame();
}
