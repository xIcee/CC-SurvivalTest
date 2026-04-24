/*
    health.c
    Survival health and damage system.

    - manages health, air, and invulnerability frames
    - environment damage (lava, drowning, falling)
    - hurt effects (camera shake, tilt)
    - death sequence and respawn logic
*/

#include "Block.h"
#include "Camera.h"
#include "ccst_api.h"
#include "Entity.h"
#include "Event.h"
#include "ExtMath.h"
#include "EntityComponents.h"
#include "Game.h"
#include "Graphics.h"
#include "Options.h"
#include "Physics.h"
#include "Server.h"
#include "String_.h"
#include "Vectors.h"
#include "World.h"
#include "health.h"
#include "deathmsg.h"
#include "motd.h"
#include "policy.h"
#include "blockbreak.h"
#include "deathscreen.h"
#include "fakeinventory.h"
#include "score.h"
#include <math.h>

// liquid expansion for collision checks
static const Vec3 ccst_liqExpand = { 0.25f / 16.0f, 0.0f / 16.0f, 0.25f / 16.0f };

static cc_bool CCST_IsWaterCollide(BlockID b) {
	return Blocks.ExtendedCollide[b] == COLLIDE_WATER;
}

static cc_bool CCST_IsLavaCollide(BlockID b) {
	return Blocks.ExtendedCollide[b] == COLLIDE_LAVA;
}

static cc_bool CCST_EntityTouchesAnyWater(struct Entity* e) {
	struct AABB bounds;
	AABB_Make(&bounds, &e->Position, &e->Size);
	Vec3_Add(&bounds.Min, &bounds.Min, &ccst_liqExpand);
	Vec3_Add(&bounds.Max, &bounds.Max, &ccst_liqExpand);
	return Entity_TouchesAny(&bounds, CCST_IsWaterCollide);
}

static cc_bool CCST_EntityTouchesAnyLava(struct Entity* e) {
	struct AABB bounds;
	AABB_Make(&bounds, &e->Position, &e->Size);
	Vec3_Add(&bounds.Min, &bounds.Min, &ccst_liqExpand);
	Vec3_Add(&bounds.Max, &bounds.Max, &ccst_liqExpand);
	return Entity_TouchesAny(&bounds, CCST_IsLavaCollide);
}

// hurt effects and constants
#define CCST_SHAKE_HURT_DURATION 10.0f 
#define CCST_SHAKE_ROLL_MAX_DEG  14.0f 
#define CCST_GAME_TICK_HZ        20.0f

#define CCST_VEL_DEADBAND      0.06f
#define CCST_VEL_SQRT_K        0.95f 
#define CCST_VEL_HEARTS_MAX    3.5f  
#define CCST_VEL_FX_MIN_EXCESS 0.035f 

#define CCST_PEER_KNOCKBACK_MCP_F1      0.4f
#define CCST_PEER_KNOCKBACK_VEL_SCALE   0.375f 

#define CCST_DEATH_DELAY_TICKS 20
#define CCST_B173_DEATH_FOV_K 500.0f
#define CCST_DEATH_FX_TILT_DEG 18.0f 

static int ccst_health;
static int ccst_air;
static int ccst_drown_hurt_cd; 
static int ccst_invuln;     
static int ccst_lastDamage; 
static int ccst_lastHealth;
static float ccst_lastShakePitch, ccst_lastShakeYaw;
static float ccst_hurt_cam_remain; 
static float ccst_hurt_dir_deg;    
static int  ccst_attacker_id;    
static int  ccst_attacker_ticks; 
static cc_bool ccst_fall_snap_inited;
static cc_bool ccst_prev_on_ground;
static float ccst_prev_vel_y;
static int ccst_fall_suppress_ticks;
static cc_bool ccst_fall_suppress_until_land;
static cc_bool ccst_survival_mode; 
static cc_bool ccst_vel_fx_cleanup_lasth; 

static int ccst_respawnLock;
static int ccst_deathTime;        
static float ccst_deathPartial;   
static int ccst_savedFov;
static cc_bool ccst_savedFov_valid;
static float ccst_pendingZoomFov; 
static cc_bool ccst_health_tick_active;

// hack override management
static cc_bool ccst_hax_override_active;
static cc_bool ccst_hax_saved_enabled;

static void CCST_HaxOverride_Save(struct HacksComp* h) {
	ccst_hax_saved_enabled = h->Enabled;
}

static cc_bool CCST_HaxTok(const cc_string* s, const char* tok) {
	return s && String_ContainsConst(s, tok);
}

static void CCST_HacksApplyPerms(struct HacksComp* h);

static void CCST_HacksParseFlag(const cc_string* flags, const char* include, const char* exclude, cc_bool* target) {
	if (!target) return;
	if (CCST_HaxTok(flags, include))      *target = true;
	else if (CCST_HaxTok(flags, exclude)) *target = false;
}

static float CCST_HacksParseFlagFloat(const cc_string* flags, const char* key, float defaultValue) {
	int i, keyLen;
	if (!flags || !key) return defaultValue;
	keyLen = String_CalcLen(key, 64);
	for (i = 0; i + keyLen <= flags->length; i++) {
		cc_string sub = String_UNSAFE_Substring(flags, i, keyLen);
		if (!String_CaselessEqualsConst(&sub, key)) continue;
		{
			cc_string raw = String_UNSAFE_SubstringAt(flags, i + keyLen);
			int end = String_IndexOf(&raw, ' ');
			float value;
			if (end >= 0) raw.length = end;
			if (!raw.length) return defaultValue;
			if (!Convert_ParseFloat(&raw, &value)) return defaultValue;
			return value;
		}
	}
	return defaultValue;
}

static int CCST_HacksParseFlagInt(const cc_string* flags, const char* key, int defaultValue) {
	int i, keyLen;
	if (!flags || !key) return defaultValue;
	keyLen = String_CalcLen(key, 64);
	for (i = 0; i + keyLen <= flags->length; i++) {
		cc_string sub = String_UNSAFE_Substring(flags, i, keyLen);
		if (!String_CaselessEqualsConst(&sub, key)) continue;
		{
			cc_string raw = String_UNSAFE_SubstringAt(flags, i + keyLen);
			int end = String_IndexOf(&raw, ' ');
			int value;
			if (end >= 0) raw.length = end;
			if (!raw.length) return defaultValue;
			if (!Convert_ParseInt(&raw, &value)) return defaultValue;
			return value;
		}
	}
	return defaultValue;
}


static void CCST_HacksRecheckFlagsLocal(struct HacksComp* h) {
	const cc_string* s;
	cc_bool hax;
	if (!h) return;
	s = &h->HacksFlags;

	hax = !CCST_HaxTok(s, "-hax");

	h->CanAnyHacks       = hax;
	h->CanFly            = hax;
	h->CanNoclip         = hax;
	h->CanRespawn        = hax;
	h->CanSpeed          = hax;
	h->CanPushbackBlocks = hax;
	h->CanUseThirdPerson = hax;
	h->CanSeeAllNames    = hax && h->IsOp;
	h->CanBePushed       = true;

	CCST_HacksParseFlag(s, "+fly",         "-fly",         &h->CanFly);
	CCST_HacksParseFlag(s, "+noclip",      "-noclip",      &h->CanNoclip);
	CCST_HacksParseFlag(s, "+speed",       "-speed",       &h->CanSpeed);
	CCST_HacksParseFlag(s, "+respawn",     "-respawn",     &h->CanRespawn);
	CCST_HacksParseFlag(s, "+push",        "-push",        &h->CanBePushed);
	CCST_HacksParseFlag(s, "+thirdperson", "-thirdperson", &h->CanUseThirdPerson);
	CCST_HacksParseFlag(s, "+names",       "-names",       &h->CanSeeAllNames);

	h->BaseHorSpeed = CCST_HacksParseFlagFloat(s, "horspeed=", h->BaseHorSpeed > 0.0f ? h->BaseHorSpeed : 1.0f);
	h->MaxHorSpeed  = CCST_HacksParseFlagFloat(s, "maxspeed=", h->MaxHorSpeed  > 0.0f ? h->MaxHorSpeed  : 1.0f);
	h->MaxJumps     = CCST_HacksParseFlagInt(s,   "jumps=",    h->MaxJumps     > 0   ? h->MaxJumps     : 1);

	CCST_HacksApplyPerms(h);
}

static void CCST_HacksApplyPerms(struct HacksComp* h) {
	if (!h) return;

	if (!h->CanFly || !h->Enabled) {
		h->Flying = false;
		h->FlyingDown = false;
		h->FlyingUp   = false;
	}
	if (!h->CanNoclip || !h->Enabled) {
		h->Noclip = false;
	}
	if (!h->CanSpeed || !h->Enabled) {
		h->Speeding = false;
		h->HalfSpeeding = false;
	}

	h->CanDoubleJump = h->Enabled && h->CanSpeed;
	h->Floating      = h->Noclip || h->Flying;
}

static void CCST_ApplySmoothFov(float desiredFovDeg) {
	struct Matrix proj;
	float baseFovDeg, baseFovRad, desiredFovRad, scale;

	if (!Camera.Active) return;
	if (desiredFovDeg < 1.0f) desiredFovDeg = 1.0f;
	if (desiredFovDeg > 179.0f) desiredFovDeg = 179.0f;

	Camera.Active->GetProjection(&proj);

	baseFovDeg  = (float)Camera.Fov;
	baseFovRad  = baseFovDeg * MATH_DEG2RAD;
	desiredFovRad = desiredFovDeg * MATH_DEG2RAD;

	scale = tanf(baseFovRad * 0.5f) / tanf(desiredFovRad * 0.5f);
	if (!isfinite(scale)) return;

	proj.row1.x *= scale;
	proj.row2.y *= scale;

	Gfx.Projection = proj;
	Gfx_LoadMatrix(MATRIX_PROJ, &Gfx.Projection);
}

static void CCST_RestoreFov(void) {
	if (!ccst_savedFov_valid) return;
	
	if (Camera.Active) {
		Camera.Active->GetProjection(&Gfx.Projection);
		Gfx_LoadMatrix(MATRIX_PROJ, &Gfx.Projection);
	}
	ccst_savedFov_valid = false;
}

void CCST_Health_ApplyZoomForNextFrame(void) {
	if (!Camera.Active) return;
	if (ccst_pendingZoomFov >= 0.0f) {
		CCST_ApplySmoothFov(ccst_pendingZoomFov);
	} else if (ccst_savedFov_valid) {
		CCST_RestoreFov();
	}
}

static cc_bool CCST_CameraInWater(void) {
	const Vec3* pos = &Camera.CurrentPos;
	int ix = (int)floorf(pos->x);
	int iy = (int)floorf(pos->y);
	int iz = (int)floorf(pos->z);
	BlockID b;

	if (ix < 0 || iy < 0 || iz < 0 || ix > World.MaxX || iy > World.MaxY || iz > World.MaxZ) return false;
	b = World_GetBlock(ix, iy, iz);
	return Blocks.ExtendedCollide[b] == COLLIDE_WATER;
}

static void CCST_ApplyHurtFxOnly(cc_bool randomHurtDir, float hurtDirDeg);

static void CCST_Shake_Clear(struct Entity* e) {
	struct LocationUpdate u;

	if (!e->VTABLE) return;
	if (fabsf(ccst_lastShakePitch) < 0.001f && fabsf(ccst_lastShakeYaw) < 0.001f) {
		ccst_lastShakePitch = ccst_lastShakeYaw = 0.0f;
		ccst_hurt_cam_remain = 0.0f;
		return;
	}
	u.flags = LU_HAS_PITCH | LU_HAS_YAW;
	u.pitch = e->next.pitch - ccst_lastShakePitch;
	u.yaw   = e->next.yaw   - ccst_lastShakeYaw;
	e->VTABLE->SetLocation(e, &u);
	ccst_lastShakePitch = ccst_lastShakeYaw = 0.0f;
	ccst_hurt_cam_remain = 0.0f;
}


static void CCST_ApplyHurtCore(int dmg, cc_bool randomHurtDir, float hurtDirDeg) {
	RNGState rnd;
	int pre;
	int excess;
	CCST_DeathCause death_cause;
	int death_inciter;

	if (dmg <= 0) {
		CCST_DeathMsg_AbortPending();
		return;
	}
	ccst_vel_fx_cleanup_lasth = false;
	if (CCST_Health_IsDeadOrDying()) {
		CCST_DeathMsg_AbortPending();
		return;
	}

	if (ccst_invuln > CCST_INVULN_HALF_TICKS) {
		if (CCST_Motd_IsGodMode()) {
			CCST_DeathMsg_AbortPending();
			return;
		}
		if (dmg <= ccst_lastDamage) {
			CCST_DeathMsg_AbortPending();
			return;
		}
		CCST_DeathMsg_ConsumePending(&death_cause, &death_inciter);
		excess = dmg - ccst_lastDamage;
		ccst_lastDamage = dmg;
		pre = ccst_health;
		ccst_health -= excess;
		if (ccst_health < 0) ccst_health = 0;
	
		if (ccst_health <= 0 && pre > 0) {
			ccst_health = 0;
			ccst_invuln = 0;
			CCST_DeathMsg_ShowAndTransmit(death_cause, death_inciter);
		}
		return;
	}

	if (CCST_Motd_IsGodMode()) {
		CCST_DeathMsg_AbortPending();
		CCST_ApplyHurtFxOnly(randomHurtDir, hurtDirDeg);
		return;
	}

	CCST_DeathMsg_ConsumePending(&death_cause, &death_inciter);
	pre = ccst_health;
	ccst_health -= dmg;
	if (ccst_health < 0) ccst_health = 0;
	ccst_lastHealth = pre;
	ccst_lastDamage = dmg;
	ccst_invuln = CCST_INVULN_TICKS;

	if (randomHurtDir) {
		Random_Seed(&rnd, (int)(Game.Time * 1000.0) ^ (pre * 1103515245));
		ccst_hurt_dir_deg = Random_Next(&rnd, 2) ? 180.0f : 0.0f;
	} else {
		float d = hurtDirDeg;
		while (d < 0.0f) d += 360.0f;
		while (d >= 360.0f) d -= 360.0f;
		ccst_hurt_dir_deg = d;
	}
	ccst_hurt_cam_remain = CCST_SHAKE_HURT_DURATION;

	if (ccst_health <= 0) {
		struct LocalPlayer* pdeath;

		ccst_health = 0;
		pdeath = Entities.CurPlayer;
		/* CPE -respawn: ClassiCube blocks BIND_RESPAWN. Skip death screen entirely and don't
		   local respawn since server policy says we cant (no teleport). Everything else is the same  */
		if (pdeath && !pdeath->Hacks.CanRespawn) {
			int finalScore = CCST_Score_Get();

			CCST_Score_Reset();
			CCST_Inv_ClearAll();
			CCST_Health_RespawnFromDeath();
			CCST_DeathMsg_ShowAndTransmit(death_cause, death_inciter);
			CCST_Chat("&7Respawn is disabled on this server.");
			Chat_Add1("&7Stats cleared. &fScore: &e%i", &finalScore);
			return;
		}

		CCST_DeathMsg_ShowAndTransmit(death_cause, death_inciter);

		/* Show overlay immediately, but lock out respawn briefly. */
		ccst_respawnLock = CCST_DEATH_DELAY_TICKS;
		ccst_deathTime = 0;
		ccst_deathPartial = 0.0f;
		ccst_pendingZoomFov = -1.0f;
		ccst_savedFov_valid = false;
		CCST_Deathscreen_Begin(CCST_Score_Get());
	}
}

static void CCST_ApplyHurtFxOnly(cc_bool randomHurtDir, float hurtDirDeg) {
	RNGState rnd;

	if (CCST_Health_IsDeadOrDying()) return;
	if (ccst_invuln > 0) return;
	ccst_vel_fx_cleanup_lasth = true;
	ccst_invuln = CCST_INVULN_TICKS;

	if (randomHurtDir) {
		Random_Seed(&rnd, (int)(Game.Time * 1000.0) ^ (ccst_health * 1103515245));
		ccst_hurt_dir_deg = Random_Next(&rnd, 2) ? 180.0f : 0.0f;
	} else {
		float d = hurtDirDeg;
		while (d < 0.0f) d += 360.0f;
		while (d >= 360.0f) d -= 360.0f;
		ccst_hurt_dir_deg = d;
	}
	ccst_hurt_cam_remain = CCST_SHAKE_HURT_DURATION;
	ccst_lastHealth = ccst_health + 2;
	if (ccst_lastHealth > CCST_MAX_HEALTH + 2)
		ccst_lastHealth = CCST_MAX_HEALTH + 2;
}

static int CCST_Death_EnvironmentInciter(void) {
	if (ccst_attacker_id >= 0 && ccst_attacker_ticks > 0) return ccst_attacker_id;
	return -1;
}

static void CCST_HurtEx(int dmg, CCST_DeathCause cause) {
	CCST_DeathMsg_SetNextCause(cause, CCST_Death_EnvironmentInciter());
	CCST_ApplyHurtCore(dmg, true, 0.0f);
}


static void CCST_Attacker_Set(int id) {
	ccst_attacker_id    = id;
	ccst_attacker_ticks = CCST_ATTACKER_TICKS;
}

static void CCST_Attacker_Clear(void) {
	ccst_attacker_id    = -1;
	ccst_attacker_ticks = 0;
}

static void CCST_Attacker_Tick(void) {
	if (ccst_attacker_id < 0) return;
	if (ccst_attacker_ticks > 0) ccst_attacker_ticks--;
	if (ccst_attacker_ticks == 0) CCST_Attacker_Clear();
}

void CCST_Health_GetAttackerName(cc_string* out) {
	cc_string raw;
	struct Entity* e;

	if (ccst_attacker_id < 0) return;
	if (TabList.NameOffsets[ccst_attacker_id]) {
		raw = TabList_UNSAFE_GetPlayer(ccst_attacker_id);
		String_AppendColorless(out, &raw);
		return;
	}

	e = Entities.List[ccst_attacker_id];
	if (e && e->NameRaw[0]) {
		int len = String_CalcLen(e->NameRaw, STRING_SIZE);
		raw = String_Init(e->NameRaw, len, STRING_SIZE);
		String_AppendColorless(out, &raw);
	}
}

int CCST_Health_GetAttackerId(void) { return ccst_attacker_id; }

static cc_bool CCST_AABB_Intersects(const struct AABB* a, const struct AABB* b) {
	return
		a->Max.x >= b->Min.x && a->Min.x <= b->Max.x &&
		a->Max.y >= b->Min.y && a->Min.y <= b->Max.y &&
		a->Max.z >= b->Min.z && a->Min.z <= b->Max.z;
}

static float CCST_Attacker_HurtDir(const struct Entity* attacker, const struct Entity* player) {
	float dx = player->Position.x - attacker->Position.x;
	float dz = player->Position.z - attacker->Position.z;
	float yawR = player->Yaw * MATH_DEG2RAD;
	float fwdX = -sinf(yawR);
	float fwdZ =  cosf(yawR);
	float rel  = (atan2f(dx, dz) - atan2f(fwdX, fwdZ)) * MATH_RAD2DEG;
	while (rel <    0.0f) rel += 360.0f;
	while (rel >= 360.0f) rel -= 360.0f;
	return rel;
}

void CCST_Health_ApplyPeerMeleeHit(int attackerEntityId) {
	struct Entity* attacker;
	struct Entity* player;
	float hurtDir;
	float dx, dz, len, f1;

	int preHp;

	if (attackerEntityId < 0 || attackerEntityId >= ENTITIES_SELF_ID) return;
	if (!CCST_Policy_PluginEnabled()) return;
	if (!CCST_Health_IsSurvivalActive()) return;
	if (CCST_Health_IsDeadOrDying()) return;

	attacker = Entities.List[attackerEntityId];
	player   = Entities.CurPlayer ? &Entities.CurPlayer->Base : NULL;
	if (!attacker || !player) return;

	CCST_Attacker_Set(attackerEntityId);
	hurtDir = CCST_Attacker_HurtDir(attacker, player);
	preHp   = ccst_health;
	
	CCST_DeathMsg_SetNextCause(CCST_DEATH_CAUSE_PLAYER, attackerEntityId);
	CCST_ApplyHurtCore(2, false, hurtDir);
	
	if (ccst_health >= preHp) return;

	f1 = CCST_PEER_KNOCKBACK_MCP_F1 * CCST_PEER_KNOCKBACK_VEL_SCALE;

	dx = attacker->Position.x - player->Position.x;
	dz = attacker->Position.z - player->Position.z;
	len = sqrtf(dx * dx + dz * dz);
	if (len < 1e-4f) {
		dx = 0.01f;
		dz = 0.0f;
		len = 0.01f;
	}
	player->Velocity.x = player->Velocity.x * 0.5f - (dx / len) * f1;
	player->Velocity.y = player->Velocity.y * 0.5f + f1;
	if (player->Velocity.y > CCST_PEER_KNOCKBACK_MCP_F1) player->Velocity.y = CCST_PEER_KNOCKBACK_MCP_F1;
	player->Velocity.z = player->Velocity.z * 0.5f - (dz / len) * f1;
}


#define CCST_CONTACT_DMG(pushStrength) \
	((int)floorf((pushStrength) * 2.0f) < 1 ? 1 : \
	 (int)floorf((pushStrength) * 2.0f) > CCST_MAX_HEALTH ? CCST_MAX_HEALTH : \
	 (int)floorf((pushStrength) * 2.0f))


static void CCST_ContactDamage_Tick(const struct Entity* player) {
	struct AABB playerBB, entityBB;
	struct Entity* e;
	int id, dmg;
	float hurtDir;

	AABB_Make(&playerBB, &player->Position, &player->Size);

	for (id = 0; id < ENTITIES_SELF_ID; id++) {
		e = Entities.List[id];
		if (!e || e->PushStrength <= 0.0f) continue;

		AABB_Make(&entityBB, &e->Position, &e->Size);
		if (!CCST_AABB_Intersects(&playerBB, &entityBB)) continue;

		dmg = CCST_CONTACT_DMG(e->PushStrength);
		hurtDir = CCST_Attacker_HurtDir(e, player);

		CCST_Attacker_Set(id);
		if (id >= 0 && id < ENTITIES_SELF_ID && TabList.NameOffsets[id])
			CCST_DeathMsg_SetNextCause(CCST_DEATH_CAUSE_PLAYER, id);
		else
			CCST_DeathMsg_SetNextCause(CCST_DEATH_CAUSE_MOB, id);
		CCST_ApplyHurtCore(dmg, false, hurtDir);
		break;
	}
}

void CCST_Health_OnVelocityImpulse(const Vec3* imp, float playerYawDeg) {
	float mag, excess, hearts, hx, hz, len, yawR, rel;
	int dmg;
	cc_bool randomDir;

	if (!imp) return;
	if (!CCST_Policy_PluginEnabled()) return;
	if (!CCST_Health_IsSurvivalActive()) return;
	if (CCST_Health_IsDeadOrDying()) return;

	mag = sqrtf(imp->x * imp->x + imp->y * imp->y + imp->z * imp->z);
	if (mag < CCST_VEL_DEADBAND) return;

	excess = mag - CCST_VEL_DEADBAND;
	hearts = CCST_VEL_SQRT_K * sqrtf(excess);
	if (hearts > CCST_VEL_HEARTS_MAX) hearts = CCST_VEL_HEARTS_MAX;
	
	dmg = (int)floorf(hearts * 2.0f);
	if (dmg < 0) dmg = 0;

	hx = -imp->x;
	hz = -imp->z;
	len = sqrtf(hx * hx + hz * hz);
	if (len < 1e-3f) {
		randomDir = true;
		rel = 0.0f;
	} else {
		float sinA, cosA, fwdX, fwdZ;
		randomDir = false;
		yawR = playerYawDeg * MATH_DEG2RAD;
		sinA = sinf(yawR);
		cosA = cosf(yawR);
		fwdX = -sinA;
		fwdZ = cosA;
		rel = (atan2f(hx, hz) - atan2f(fwdX, fwdZ)) * MATH_RAD2DEG;
		while (rel < 0.0f) rel += 360.0f;
		while (rel >= 360.0f) rel -= 360.0f;
	}

	if (dmg > 0) {
		CCST_DeathMsg_SetNextCause(CCST_DEATH_CAUSE_GENERIC, -1);
		CCST_ApplyHurtCore(dmg, randomDir, rel);
	}
	else if (excess >= CCST_VEL_FX_MIN_EXCESS)
		CCST_ApplyHurtFxOnly(randomDir, rel);
}

void CCST_Health_FoodApply(int deltaHalfHearts) {
	if (deltaHalfHearts == 0) return;
	if (!CCST_Health_IsSurvivalActive()) return;
	if (CCST_Health_IsDeadOrDying()) return;

	if (deltaHalfHearts > 0) {
		ccst_health += deltaHalfHearts;
		if (ccst_health > CCST_MAX_HEALTH) ccst_health = CCST_MAX_HEALTH;
		if (ccst_lastHealth < ccst_health) ccst_lastHealth = ccst_health;
	} else {
		CCST_HurtEx(-deltaHalfHearts, CCST_DEATH_CAUSE_POISON);
	}
}

void CCST_Health_EnforceSurvivalHacks(void) {
	struct LocalPlayer* p;

	if (!CCST_Policy_PluginEnabled()) return;

	p = Entities.CurPlayer;

	if (!CCST_Health_IsSurvivalActive()) {
		if (p && p->Hacks.Enabled) {
			p->Hacks.PushbackPlacing = Options_GetBool(OPT_PUSHBACK_PLACING, false);
			p->Hacks.FullBlockStep   = Options_GetBool(OPT_FULL_BLOCK_STEP, false);
		}
		
		if (p && ccst_hax_override_active) {
			CCST_HacksRecheckFlagsLocal(&p->Hacks);
			p->Hacks.Enabled = ccst_hax_saved_enabled;
			ccst_hax_override_active = false;
			CCST_HacksApplyPerms(&p->Hacks);
		}
		return;
	}

	// kinda like the opposite of hacking by making our policy stricter... 
	// if server enforces survival AND still has policy flags, then we apply them anyways.
	if (p && !Server.IsSinglePlayer) {
		struct HacksComp* h = &p->Hacks;
		const cc_string* s = &h->HacksFlags;
		cc_bool allowFly, allowNoclip, allowSpeed, allowThird, allowNames;
		cc_bool allowAll;
		cc_bool any;

		if (!ccst_hax_override_active) {
			CCST_HaxOverride_Save(h);
			ccst_hax_override_active = true;
		}

		// without explicit +survival token: survival-nohax baseline wins and we ignore server hacks flags entirely.
		// with +survival token: apply a whitelist driven by the server flags.
		if (!CCST_Motd_HasSurvivalToken()) {
			h->Enabled           = false;
			h->CanAnyHacks       = false;
			h->CanFly            = false;
			h->CanNoclip         = false;
			h->CanSpeed          = false;
			h->CanUseThirdPerson = false;
			h->CanSeeAllNames    = false;
			h->CanDoubleJump     = false;
			CCST_HacksApplyPerms(h);
			return;
		}

		allowAll = CCST_HaxTok(s, "+hax") && !CCST_HaxTok(s, "-hax");

		allowFly    = allowAll && !CCST_HaxTok(s, "-fly");
		allowNoclip = allowAll && !CCST_HaxTok(s, "-noclip");
		allowThird  = allowAll && !CCST_HaxTok(s, "-thirdperson");
		allowNames  = allowAll && !CCST_HaxTok(s, "-names");

		if (CCST_HaxTok(s, "+fly"))         allowFly    = true;
		if (CCST_HaxTok(s, "+noclip"))      allowNoclip = true;
		if (CCST_HaxTok(s, "+thirdperson")) allowThird  = true;
		if (CCST_HaxTok(s, "+names"))       allowNames  = true;

		allowSpeed =
			(allowAll && !CCST_HaxTok(s, "-speed"))
			|| CCST_HaxTok(s, "+speed")
			|| CCST_HaxTok(s, "maxspeed=")
			|| CCST_HaxTok(s, "horspeed=")
			|| CCST_HaxTok(s, "jumps=");

		any = allowFly || allowNoclip || allowSpeed || allowThird || allowNames;

		h->Enabled           = any;
		h->CanAnyHacks       = any;
		h->CanFly            = allowFly;
		h->CanNoclip         = allowNoclip;
		h->CanSpeed          = allowSpeed;
		h->CanUseThirdPerson = allowThird;
		h->CanSeeAllNames    = allowNames;
		CCST_HacksApplyPerms(h);
	}
}

static void CCST_Health_OnEntityRemoved(void* obj, int id) {
	(void)obj;
	if (id == ccst_attacker_id) CCST_Attacker_Clear();
}

static void CCST_Health_OnHackPermsChanged(void* obj) {
	(void)obj;
	if (!CCST_Policy_PluginEnabled() || !CCST_Health_IsSurvivalActive()) return;
	if (!CCST_Deathscreen_IsActive()) return;
	if (!Entities.CurPlayer || Entities.CurPlayer->Hacks.CanRespawn) return;

	CCST_Deathscreen_ForceCancel();
	CCST_Health_RespawnFromDeath();
	CCST_Chat("Respawn was disabled on this server.");
	CCST_Chat("The survival death screen was closed.");
}

void CCST_Health_Init(void) {
	ccst_survival_mode = true;
	ccst_health_tick_active = true;
	CCST_Health_Reset();
	Event_Register_(&UserEvents.HackPermsChanged, NULL, CCST_Health_OnHackPermsChanged);
	Event_Register_(&EntityEvents.Removed, NULL, CCST_Health_OnEntityRemoved);
}

void CCST_Health_Free(void) {
	ccst_health_tick_active = false;
	Event_Unregister_(&UserEvents.HackPermsChanged, NULL, CCST_Health_OnHackPermsChanged);
	Event_Unregister_(&EntityEvents.Removed, NULL, CCST_Health_OnEntityRemoved);
}

void CCST_Health_Reset(void) {
	CCST_Deathscreen_ForceCancel();
	CCST_RestoreFov();
	ccst_health   = CCST_MAX_HEALTH;
	ccst_air      = CCST_AIR_MAX;
	ccst_drown_hurt_cd = 0;
	ccst_invuln   = 0;
	ccst_lastDamage = 0;
	ccst_lastHealth = CCST_MAX_HEALTH;
	ccst_lastShakePitch = ccst_lastShakeYaw = 0.0f;
	ccst_hurt_cam_remain = 0.0f;
	ccst_fall_snap_inited = false;
	ccst_prev_on_ground   = true;
	ccst_prev_vel_y       = 0.0f;
	ccst_vel_fx_cleanup_lasth = false;
	ccst_respawnLock = 0;
	ccst_deathTime = 0;
	ccst_deathPartial = 0.0f;
	ccst_pendingZoomFov = -1.0f;
	CCST_Attacker_Clear();
	if (World.Loaded && Entities.CurPlayer)
		CCST_Shake_Clear(&Entities.CurPlayer->Base);
}

void CCST_Health_RespawnFromDeath(void) {
	struct Entity* e;

	CCST_RestoreFov();
	ccst_vel_fx_cleanup_lasth = false;
	ccst_health   = CCST_MAX_HEALTH;
	ccst_air      = CCST_AIR_MAX;
	ccst_drown_hurt_cd = 0;
	ccst_invuln   = 0;
	ccst_lastDamage = 0;
	ccst_lastHealth = CCST_MAX_HEALTH;
	ccst_fall_snap_inited = false;
	ccst_prev_on_ground   = true;
	ccst_prev_vel_y       = 0.0f;
	ccst_hurt_cam_remain = 0.0f;
	ccst_lastShakePitch = ccst_lastShakeYaw = 0.0f;
	ccst_respawnLock = 0;
	ccst_deathTime = 0;
	ccst_deathPartial = 0.0f;
	ccst_pendingZoomFov = -1.0f;
	CCST_Attacker_Clear();
	if (World.Loaded && Entities.CurPlayer) {
		e = &Entities.CurPlayer->Base;
		CCST_Shake_Clear(e);
	}

	if (CCST_Policy_PluginEnabled() && CCST_Health_IsSurvivalActive() && World.Loaded && Entities.CurPlayer)
		CCST_ApplyHurtFxOnly(true, 0.0f);
}

cc_bool CCST_Health_IsSurvivalActive(void) {
	if (!CCST_Policy_SurvivalMechanics()) return false;
	switch (CCST_Motd_GetForcedGamemode()) {
	case CCST_MOTD_GM_CREATIVE:
		return false;
	case CCST_MOTD_GM_SURVIVAL:
		return true;
	default:
		break;
	}
	return ccst_survival_mode;
}

cc_bool CCST_Health_IsDeadOrDying(void) {
	return ccst_respawnLock > 0 || CCST_Deathscreen_IsActive();
}

cc_bool CCST_Health_CanRespawn(void) {
	struct LocalPlayer* p;

	if (!CCST_Deathscreen_IsActive() || ccst_respawnLock > 0) return false;
	p = Entities.CurPlayer;
	if (!p || !p->Hacks.CanRespawn) return false;
	return true;
}

int CCST_Health_Get(void) { return ccst_health; }

void CCST_Health_Set(int halfHearts) {
	if (halfHearts < 0) halfHearts = 0;
	if (halfHearts > CCST_MAX_HEALTH) halfHearts = CCST_MAX_HEALTH;
	ccst_health = halfHearts;
	if (ccst_lastHealth < ccst_health) ccst_lastHealth = ccst_health;
}

void CCST_Health_SuppressFallDamage(int ticks) {
	if (ticks < 0) ticks = 0;
	if (ticks > 200) ticks = 200;
	ccst_fall_suppress_ticks = ticks;
	ccst_fall_suppress_until_land = true;

	ccst_prev_on_ground = true;
	ccst_prev_vel_y     = 0.0f;
	ccst_fall_snap_inited = false;
}

int CCST_Health_GetInvulnTicks(void) { return ccst_invuln; }

int CCST_Health_GetLastHealth(void) { return ccst_lastHealth; }

int CCST_Air_Get(void) { return ccst_air; }

void CCST_Health_SetModeSurvival(cc_bool survival) {
	ccst_survival_mode = survival;
	CCST_BlockBreak_OnSurvivalStateMayHaveChanged();
}

cc_bool CCST_Health_GetModeSurvival(void) {
	return ccst_survival_mode;
}

void CCST_Health_OnFrame(float delta) {
	struct Entity* e;
	struct LocationUpdate u;
	float newP, newY, dPitch, dYaw, ratio, rollDeg, hdRad;
	float deathP, deathY;
	float baseFovF, desiredFovF, deathFrac;
	float dt_shake;

	if (!World.Loaded || !Entities.CurPlayer) return;
	e = &Entities.CurPlayer->Base;
	if (!e->VTABLE) return;
	if (!CCST_Policy_PluginEnabled()) {
		CCST_Shake_Clear(e);
		CCST_RestoreFov();
		ccst_deathTime = 0;
		ccst_deathPartial = 0.0f;
		ccst_pendingZoomFov = -1.0f;
		return;
	}

	if (!CCST_Health_IsSurvivalActive()) {
		CCST_Shake_Clear(e);
		CCST_RestoreFov();
		ccst_deathTime = 0;
		ccst_deathPartial = 0.0f;
		ccst_pendingZoomFov = -1.0f;
		return;
	}

	if (delta <= 0.0f) return;
	dt_shake = delta;
	if (dt_shake > 0.1f) dt_shake = 0.1f;

	if (CCST_Health_IsDeadOrDying()) {
		if (!ccst_savedFov_valid) {
			ccst_savedFov = Camera.Fov;
			ccst_savedFov_valid = true;
		}

		ccst_deathPartial += delta;
		if (ccst_deathPartial < 0.0f) ccst_deathPartial = 0.0f;
		{
			float partialTick = ccst_deathPartial * CCST_GAME_TICK_HZ;
			float t;
			if (partialTick > 1.0f) partialTick = 1.0f;
			t = (float)ccst_deathTime + partialTick;

			baseFovF = (float)ccst_savedFov;
			if (CCST_CameraInWater()) baseFovF = baseFovF * (60.0f / 70.0f);

			{
				float denom = (1.0f - (CCST_B173_DEATH_FOV_K / (t + CCST_B173_DEATH_FOV_K))) * 2.0f + 1.0f;
				if (denom < 0.001f) denom = 0.001f;
				desiredFovF = baseFovF / denom;
			}
		}


		ccst_pendingZoomFov = desiredFovF;

		deathFrac = 1.0f - (desiredFovF / baseFovF);
		if (deathFrac < 0.0f) deathFrac = 0.0f;
		if (deathFrac > 1.0f) deathFrac = 1.0f;
	} else {
		ccst_deathTime = 0;
		ccst_deathPartial = 0.0f;
		ccst_pendingZoomFov = -1.0f;
		deathFrac = 0.0f;
	}

	if (ccst_hurt_cam_remain > 0.0f) {
		ccst_hurt_cam_remain -= dt_shake * CCST_GAME_TICK_HZ;
		if (ccst_hurt_cam_remain < 0.0f) ccst_hurt_cam_remain = 0.0f;
	}

	ratio = ccst_hurt_cam_remain / CCST_SHAKE_HURT_DURATION;
	if (ratio < 0.0f) ratio = 0.0f;
	if (ratio > 1.0f) ratio = 1.0f;

	rollDeg = CCST_SHAKE_ROLL_MAX_DEG * sinf(MATH_PI * powf(ratio, 4.0f));
	hdRad = ccst_hurt_dir_deg * MATH_DEG2RAD;
	newP = -rollDeg * cosf(hdRad);
	newY = rollDeg * sinf(hdRad);
	if (fabsf(newP) < 0.02f) newP = 0.0f;
	if (fabsf(newY) < 0.02f) newY = 0.0f;

	deathP = CCST_DEATH_FX_TILT_DEG * deathFrac;
	deathY = 0.0f;
	newP += deathP;
	newY += deathY;

	if (newP == 0.0f && newY == 0.0f && fabsf(ccst_lastShakePitch) < 0.001f && fabsf(ccst_lastShakeYaw) < 0.001f)
		return;

	u.flags = LU_HAS_PITCH | LU_HAS_YAW;
	dPitch = newP - ccst_lastShakePitch;
	dYaw   = newY - ccst_lastShakeYaw;
	u.pitch = e->next.pitch + dPitch;
	u.yaw   = e->next.yaw   + dYaw;
	e->VTABLE->SetLocation(e, &u);

	ccst_lastShakePitch = newP;
	ccst_lastShakeYaw   = newY;
}

void CCST_Health_OnTick(struct ScheduledTask* task) {
	struct Entity* e;
	struct LocalPlayer* p;
	float impact_down, equiv_dist;
	int fall_dmg;

	(void)task;
	if (!ccst_health_tick_active) return;
	if (!World.Loaded) return;
	if (!CCST_Policy_PluginEnabled()) return;

	CCST_Attacker_Tick();
	CCST_Health_EnforceSurvivalHacks();

	if (!Entities.CurPlayer) return;

	p = Entities.CurPlayer;
	e = &p->Base;

	if (!CCST_Health_IsSurvivalActive()) {
		CCST_Shake_Clear(e);
		return;
	}

	if (CCST_Deathscreen_IsActive()) {
		if (ccst_respawnLock > 0) ccst_respawnLock--;
		ccst_deathTime++;
		ccst_deathPartial = 0.0f;
		return;
	}

	if (ccst_invuln > 0) {
		ccst_invuln--;
		if (ccst_invuln == 0) ccst_lastDamage = 0;
	}
	if (ccst_vel_fx_cleanup_lasth && ccst_invuln == 0) {
		ccst_lastHealth = ccst_health;
		ccst_vel_fx_cleanup_lasth = false;
	}

	if (CCST_EntityTouchesAnyLava(e))
		CCST_HurtEx(10, CCST_DEATH_CAUSE_LAVA);

	CCST_ContactDamage_Tick(e);

	if (CCST_CameraInWater()) {
		if (ccst_air > 0) {
			ccst_air--;
			ccst_drown_hurt_cd = 0;
		} else {
			if (ccst_drown_hurt_cd == 0) {
				CCST_HurtEx(2, CCST_DEATH_CAUSE_DROWN);
				ccst_drown_hurt_cd = CCST_DROWN_DAMAGE_INTERVAL_TICKS - 1;
			} else {
				ccst_drown_hurt_cd--;
			}
		}
	} else {
		ccst_air = CCST_AIR_MAX;
		ccst_drown_hurt_cd = 0;
	}

	if (ccst_fall_suppress_until_land && ccst_fall_snap_inited && !ccst_prev_on_ground && e->OnGround) {
		ccst_fall_suppress_until_land = false;
		ccst_fall_suppress_ticks = 0;
	} else if (ccst_fall_suppress_ticks > 0) {
		ccst_fall_suppress_ticks--;
	} else if (ccst_fall_snap_inited && !ccst_prev_on_ground && e->OnGround
	    && !p->Hacks.Flying && !p->Hacks.Noclip
	    && !CCST_EntityTouchesAnyWater(e) && !CCST_EntityTouchesAnyLava(e)) {
		impact_down = -ccst_prev_vel_y;
		if (impact_down > 0.0f) {
			equiv_dist = (impact_down * impact_down)
				/ (2.0f * CCST_FALL_GRAVITY_PER_TICK);
			fall_dmg = (int)ceilf(equiv_dist - CCST_FALL_SAFE);
			if (fall_dmg > 0)
				CCST_HurtEx(fall_dmg, CCST_DEATH_CAUSE_FALL);
		}
	}

	if (ccst_lastHealth < ccst_health)
		ccst_lastHealth = ccst_health;

	ccst_prev_on_ground = e->OnGround;
	ccst_prev_vel_y     = e->Velocity.y;
	ccst_fall_snap_inited = true;
}