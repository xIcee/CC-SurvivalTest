/*
    tilefood.c
    edible sprite blocks and plant-based healing.

    - eating logic for flowers and plants
    - health effects based on texture hue/saturation
    - prevents placement while eating
*/
#include "PluginAPI.h"
#include "Audio.h"
#include "Block.h"
#include "Core.h"
#include "Entity.h"
#include "EntityComponents.h"
#include "Event.h"
#include "Camera.h"
#include "Constants.h"
#include "Funcs.h"
#include "Gui.h"
#include "Options.h"
#include "Input.h"
#include "Inventory.h"
#include "Physics.h"
#include "Picking.h"
#include "Vectors.h"
#include "World.h"
#include "fakeinventory.h"
#include "health.h"
#include "policy.h"
#include "texturenoise.h"
#include "ccst_binds.h"

static cc_bool ccst_tf_just_placed_edible;
static IVec3 ccst_tf_place_coord;
static BlockID ccst_tf_place_new;

static void CCST_TF_ClearPick(struct RayTracer* t) {
	IVec3 bad = { -1, -1, -1 };
	t->pos           = bad;
	t->translatedPos = bad;
	t->valid         = false;
	t->block         = BLOCK_AIR;
	t->closest       = FACE_COUNT;
}

static cc_bool CCST_TF_AnyScreenBlocksWorld(void) {
	int i;
	for (i = 0; i < Gui.ScreensCount; i++) {
		if (Gui.Screens[i]->blocksWorld) return true;
	}
	return false;
}

// mirror internal raytracer
static void CCST_TF_UpdatePick(struct RayTracer* out) {
	CCST_TF_ClearPick(out);
	if (!Camera.Active) return;
	if (CCST_TF_AnyScreenBlocksWorld()) return;
	Camera.Active->GetPickedBlock(out);
}

static cc_bool CCST_TF_AabbIntersects(const struct AABB* a, const struct AABB* b) {
	return
		a->Max.x >= b->Min.x && a->Min.x <= b->Max.x &&
		a->Max.y >= b->Min.y && a->Min.y <= b->Max.y &&
		a->Max.z >= b->Min.z && a->Min.z <= b->Max.z;
}

static cc_bool CCST_TF_TouchesSolid(BlockID b) {
	return Blocks.Collide[b] == COLLIDE_SOLID;
}

// pushback on placement
static cc_bool CCST_TF_PushbackPlace(struct RayTracer* pick, struct AABB* blockBB) {
	struct Entity* p        = &Entities.CurPlayer->Base;
	struct HacksComp* hacks = &Entities.CurPlayer->Hacks;
	Face closestFace;
	cc_bool insideMap;
	Vec3 pos = p->Position;
	struct AABB playerBB;
	struct LocationUpdate update;

	closestFace = pick->closest;
	if (closestFace == FACE_XMAX) {
		pos.x = blockBB->Max.x + 0.5f;
	} else if (closestFace == FACE_ZMAX) {
		pos.z = blockBB->Max.z + 0.5f;
	} else if (closestFace == FACE_XMIN) {
		pos.x = blockBB->Min.x - 0.5f;
	} else if (closestFace == FACE_ZMIN) {
		pos.z = blockBB->Min.z - 0.5f;
	} else if (closestFace == FACE_YMAX) {
		pos.y = blockBB->Min.y + 1 + ENTITY_ADJUSTMENT;
	} else if (closestFace == FACE_YMIN) {
		pos.y = blockBB->Min.y - p->Size.y - ENTITY_ADJUSTMENT;
	}

	insideMap =
		pos.x > 0.0f && pos.y >= 0.0f && pos.z > 0.0f &&
		pos.x < World.Width && pos.z < World.Length;
	if (!insideMap) return false;

	AABB_Make(&playerBB, &pos, &p->Size);
	if (!hacks->Noclip && Entity_TouchesAny(&playerBB, CCST_TF_TouchesSolid))
		return false;

	update.flags = LU_HAS_POS | LU_POS_ABSOLUTE_INSTANT;
	update.pos   = pos;
	p->VTABLE->SetLocation(p, &update);
	return true;
}

static cc_bool CCST_TF_IntersectsOthers(Vec3 pos, BlockID block) {
	struct AABB blockBB, entityBB;
	struct Entity* e;
	int id;

	Vec3_Add(&blockBB.Min, &pos, &Blocks.MinBB[block]);
	Vec3_Add(&blockBB.Max, &pos, &Blocks.MaxBB[block]);

	for (id = 0; id < ENTITIES_MAX_COUNT; id++) {
		e = Entities.List[id];
		if (!e || e == &Entities.CurPlayer->Base) continue;

		AABB_Make(&entityBB, &e->Position, &e->Size);
		entityBB.Min.y += 1.0f / 32.0f;
		if (CCST_TF_AabbIntersects(&entityBB, &blockBB)) return true;
	}
	return false;
}

static cc_bool CCST_TF_CheckIsFree(struct RayTracer* pick, BlockID block) {
	struct Entity* p        = &Entities.CurPlayer->Base;
	struct HacksComp* hacks = &Entities.CurPlayer->Hacks;
	Vec3 pos, nextPos;
	struct AABB blockBB, playerBB;
	struct LocationUpdate update;

	if (Blocks.Collide[block] != COLLIDE_SOLID) return true;

	pos.x = (float)pick->translatedPos.x;
	pos.y = (float)pick->translatedPos.y;
	pos.z = (float)pick->translatedPos.z;
	if (CCST_TF_IntersectsOthers(pos, block)) return false;

	nextPos = p->next.pos;
	Vec3_Add(&blockBB.Min, &pos, &Blocks.MinBB[block]);
	Vec3_Add(&blockBB.Max, &pos, &Blocks.MaxBB[block]);

	AABB_Make(&playerBB, &p->Position, &p->Size);
	playerBB.Min.y = min(nextPos.y, playerBB.Min.y);

	if (hacks->Noclip || !CCST_TF_AabbIntersects(&playerBB, &blockBB)) return true;
	if (hacks->CanPushbackBlocks && hacks->PushbackPlacing && hacks->Enabled)
		return CCST_TF_PushbackPlace(pick, &blockBB);

	playerBB.Min.y += 0.25f + ENTITY_ADJUSTMENT;
	if (CCST_TF_AabbIntersects(&playerBB, &blockBB)) return false;

	nextPos.y = pos.y + Blocks.MaxBB[block].y + ENTITY_ADJUSTMENT;
	update.flags = LU_HAS_POS | LU_POS_ABSOLUTE_INSTANT;
	update.pos   = nextPos;
	p->VTABLE->SetLocation(p, &update);
	return true;
}

static cc_bool CCST_TF_CanPick(BlockID block) {
	cc_bool breakLiquids = Options_GetBool(OPT_MODIFIABLE_LIQUIDS, false);
	if (Blocks.Draw[block] == DRAW_GAS) return false;
	if (Blocks.Draw[block] == DRAW_SPRITE) return true;
	return Blocks.Collide[block] != COLLIDE_LIQUID || breakLiquids;
}

static cc_bool CCST_TF_PlaceWouldApply(struct RayTracer* pick, BlockID block) {
	IVec3 pos;
	BlockID old;

	pos = pick->translatedPos;
	if (!pick->valid || !World_Contains(pos.x, pos.y, pos.z))
		return false;

	old = World_GetBlock(pos.x, pos.y, pos.z);
	if (CCST_TF_CanPick(old) || !Blocks.CanPlace[block])
		return false;
	if (Blocks.Draw[block] == DRAW_GAS && Blocks.Draw[old] != DRAW_GAS)
		return false;
	if (Blocks.Collide[old] == COLLIDE_NONE && !Blocks.CanDelete[old])
		return false;
	if (!CCST_TF_CheckIsFree(pick, block))
		return false;
	return true;
}

static cc_bool CCST_TF_IsEdibleBlock(BlockID b) {
	if (b == BLOCK_AIR || b >= BLOCK_COUNT) return false;
	if (Blocks.Draw[b] != DRAW_SPRITE) return false;
	if (Blocks.DigSounds[b] != SOUND_GRASS) return false;
	return true;
}

// determine health effect from color properties
static int CCST_TF_EffectFromHueSat(float hueDeg, float sat) {
	int heal;

	if (sat < 0.18f)
		return 1;
	if (hueDeg < 0.0f)
		return 1;

	if (hueDeg <= 22.0f || hueDeg >= 338.0f)
		return -2;
	if (hueDeg >= 292.0f && hueDeg < 338.0f)
		return -1;
	if (hueDeg >= 78.0f && hueDeg <= 158.0f) {
		heal = 2 + (int)((158.0f - hueDeg) / 80.0f * 3.0f + 0.5f);
		if (heal > 4) heal = 4;
		if (heal < 2) heal = 2;
		return heal;
	}
	if (hueDeg >= 38.0f && hueDeg < 78.0f)
		return 2;
	return 1;
}

static void CCST_TF_OnUserBlockChanged(void* obj, IVec3 coords, BlockID oldBlock, BlockID newBlock) {
	(void)obj;
	(void)oldBlock;
	if (newBlock == BLOCK_AIR || Blocks.Draw[newBlock] == DRAW_GAS) return;
	if (!CCST_TF_IsEdibleBlock(newBlock)) return;
	ccst_tf_just_placed_edible = true;
	ccst_tf_place_coord = coords;
	ccst_tf_place_new     = newBlock;
}

static void CCST_TF_OnInputDown2(void* obj, int key, cc_bool repeating, struct InputDevice* device) {
	struct RayTracer pick;
	BlockID held, placeBlock;
	float hue, sat;
	int effect;

	(void)obj;
	if (EventAPIVersion < 4) return;
	if (repeating) return;
	if (Gui.InputGrab) return;
	if (!World.Loaded || !Entities.CurPlayer) return;
	if (!CCST_Policy_PluginEnabled() || !CCST_Policy_TileFoodEnabled()) return;
	if (CCST_Health_IsDeadOrDying()) return;
	if (CCST_Inv_IsOpen()) return;
	if (!CCST_InputBind_Claims(BIND_PLACE_BLOCK, key, device)) return;

	held = Inventory_SelectedBlock;
	placeBlock = held;

	if (!CCST_TF_IsEdibleBlock(held))
		return;

	CCST_TF_UpdatePick(&pick);

	if (ccst_tf_just_placed_edible) {
		cc_bool skip;
		skip = pick.valid &&
			World_Contains(pick.translatedPos.x, pick.translatedPos.y, pick.translatedPos.z) &&
			pick.translatedPos.x == ccst_tf_place_coord.x &&
			pick.translatedPos.y == ccst_tf_place_coord.y &&
			pick.translatedPos.z == ccst_tf_place_coord.z &&
			placeBlock == ccst_tf_place_new;
		ccst_tf_just_placed_edible = false;
		if (skip) return;
	}

	if (CCST_TF_PlaceWouldApply(&pick, placeBlock))
		return;

	CCST_TextureNoise_MeanHueSatForBlock(held, &hue, &sat);
	effect = CCST_TF_EffectFromHueSat(hue, sat);

	if (!CCST_Inv_TryConsumeOneSelected(held))
		return;

	CCST_Health_FoodApply(effect);
}

void CCST_Tilefood_Init(void) {
	Event_Register_(&UserEvents.BlockChanged, NULL, CCST_TF_OnUserBlockChanged);
	if (EventAPIVersion >= 4)
		Event_Register_(&InputEvents.Down2, NULL, CCST_TF_OnInputDown2);
}

void CCST_Tilefood_Free(void) {
	Event_Unregister_(&UserEvents.BlockChanged, NULL, CCST_TF_OnUserBlockChanged);
	if (EventAPIVersion >= 4)
		Event_Unregister_(&InputEvents.Down2, NULL, CCST_TF_OnInputDown2);
}
