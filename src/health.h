#ifndef CCST_HEALTH_H
#define CCST_HEALTH_H
#include "Core.h"
#include "Vectors.h"

struct ScheduledTask;

#define CCST_MAX_HEALTH 20
#define CCST_AIR_MAX 300
#define CCST_DROWN_DAMAGE_INTERVAL_TICKS 20
#define CCST_INVULN_TICKS      20
#define CCST_INVULN_HALF_TICKS 10
#define CCST_FALL_SAFE 3.0f
#define CCST_FALL_GRAVITY_PER_TICK 0.08f
#define CCST_ATTACKER_TICKS 100

void CCST_Health_Init(void);
void CCST_Health_Free(void);
void CCST_Health_Reset(void);
void CCST_Health_RespawnFromDeath(void);
void CCST_Health_OnTick(struct ScheduledTask* task);

void CCST_Health_OnFrame(float delta);
void CCST_Health_EnforceSurvivalHacks(void);
cc_bool CCST_Health_IsSurvivalActive(void);
cc_bool CCST_Health_IsDeadOrDying(void);
cc_bool CCST_Health_CanRespawn(void);

void CCST_Health_ApplyZoomForNextFrame(void);
int CCST_Health_Get(void);
void CCST_Health_Set(int halfHearts);
int CCST_Health_GetInvulnTicks(void);
int CCST_Health_GetLastHealth(void);
int CCST_Air_Get(void);
void CCST_Health_SetModeSurvival(cc_bool survival);
cc_bool CCST_Health_GetModeSurvival(void);

void CCST_Health_FoodApply(int deltaHalfHearts);
void CCST_Health_SuppressFallDamage(int ticks);
void CCST_Health_OnVelocityImpulse(const Vec3* impulseWorld, float playerYawDeg);
void CCST_Health_GetAttackerName(cc_string* out);
int CCST_Health_GetAttackerId(void);
void CCST_Health_ApplyPeerMeleeHit(int attackerEntityId);

#endif
