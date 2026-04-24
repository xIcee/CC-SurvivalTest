#include "Camera.h"
#include "Entity.h"
#include "respawn.h"

/* LocalPlayers_MoveToSpawn is not CC_API; mirror Entity.c (single local player). */
void CCST_Respawn_Player(void) {
	struct LocalPlayer* p;
	struct LocationUpdate update;

	p = Entities.CurPlayer;
	update.flags = LU_HAS_POS | LU_HAS_YAW | LU_HAS_PITCH | LU_POS_ABSOLUTE_INSTANT;
	update.pos   = p->Spawn;
	update.yaw   = p->SpawnYaw;
	update.pitch = p->SpawnPitch;

	p->Base.VTABLE->SetLocation(&p->Base, &update);
	if (update.flags & LU_HAS_POS) p->Spawn = update.pos;
	if (update.flags & LU_HAS_YAW) p->SpawnYaw = update.yaw;
	if (update.flags & LU_HAS_PITCH) p->SpawnPitch = update.pitch;

	Camera.CurrentPos = Camera.Active->GetPosition(0.0f);
}
