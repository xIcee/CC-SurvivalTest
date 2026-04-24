/*
    netshim.c
    Packet interception and CPE shims.

    - intercepts velocity control for damage/effects
    - tracks block permission changes
    - syncs hotbar changes with inventory
*/
#include "PluginAPI.h"
#include "BlockID.h"
#include "Entity.h"
#include "Event.h"
#include "Protocol.h"
#include "Server.h"
#include "Stream.h"
#include "Vectors.h"
#include "World.h"
#include "health.h"
#include "blockbreak.h"
#include "policy.h"
#include "fakeinventory.h"

static Net_Handler ccst_real_velocity;
static Net_Handler ccst_real_setBlockPerm;
static Net_Handler ccst_real_setHotbar;

// intercept CPE velocity control
static void CCST_Netshim_VelocityControl(cc_uint8* data) {
	struct LocalPlayer* p;
	Vec3 before, after;

	if (!ccst_real_velocity) return;
	p = Entities.CurPlayer;
	if (p && World.Loaded) {
		float yaw = p->Base.Yaw;
		before = p->Base.Velocity;
		ccst_real_velocity(data);
		after = p->Base.Velocity;
		if (CCST_Policy_PluginEnabled()) {
			Vec3 d;
			d.x = after.x - before.x;
			d.y = after.y - before.y;
			d.z = after.z - before.z;
			CCST_Health_OnVelocityImpulse(&d, yaw);
		}
	} else {
		ccst_real_velocity(data);
	}
}

// track per-block break permissions
static void CCST_Netshim_SetBlockPermission(cc_uint8* data) {
	BlockID block;
	cc_bool canDelete;
	int size;

	if (!ccst_real_setBlockPerm) return;

	size = Protocol.Sizes[OPCODE_SET_BLOCK_PERMISSION];
	if (size >= 5) {
		block = (BlockID)(((cc_uint16)((data[0] << 8) | data[1])) % BLOCK_COUNT);
		canDelete = data[3] != 0;
	} else {
		block = (BlockID)(data[0] % BLOCK_COUNT);
		canDelete = data[2] != 0;
	}

	CCST_BlockBreak_OnSetBlockPermission(block, canDelete);
	ccst_real_setBlockPerm(data);
}

// sync server hotbar changes
static void CCST_Netshim_SetHotbar(cc_uint8* data) {
	BlockID block;
	cc_uint8 index;
	int size;

	if (!ccst_real_setHotbar) return;

	size = Protocol.Sizes[OPCODE_SET_HOTBAR];
	if (size >= 4) {
		block = (BlockID)(((cc_uint16)((data[0] << 8) | data[1])) % BLOCK_COUNT);
		index = data[2];
	} else {
		block = (BlockID)(data[0] % BLOCK_COUNT);
		index = data[1];
	}

	ccst_real_setHotbar(data);
	CCST_Inv_OnServerSetHotbar((int)index, block);
}

// shim installer
void CCST_Netshim_Install(void) {
	Net_Handler h;

	/* Game_Version is not CC_VAR, do not read it from plugins. CPE registers this handler only when active.
	 * Velocity and SetBlockPermission are independent: do not bail out of one when the other is missing or already shimmed. */
	h = Protocol.Handlers[OPCODE_VELOCITY_CONTROL];
	if (h && h != CCST_Netshim_VelocityControl) {
		ccst_real_velocity = h;
		Net_Set(OPCODE_VELOCITY_CONTROL, CCST_Netshim_VelocityControl, Protocol.Sizes[OPCODE_VELOCITY_CONTROL]);
	}

	h = Protocol.Handlers[OPCODE_SET_BLOCK_PERMISSION];
	if (h && h != CCST_Netshim_SetBlockPermission) {
		ccst_real_setBlockPerm = h;
		Net_Set(OPCODE_SET_BLOCK_PERMISSION, CCST_Netshim_SetBlockPermission, Protocol.Sizes[OPCODE_SET_BLOCK_PERMISSION]);
	}

	h = Protocol.Handlers[OPCODE_SET_HOTBAR];
	if (h && h != CCST_Netshim_SetHotbar) {
		ccst_real_setHotbar = h;
		Net_Set(OPCODE_SET_HOTBAR, CCST_Netshim_SetHotbar, Protocol.Sizes[OPCODE_SET_HOTBAR]);
	}
}

void CCST_Netshim_Remove(void) {
	if (ccst_real_velocity && Protocol.Handlers[OPCODE_VELOCITY_CONTROL] == CCST_Netshim_VelocityControl) {
		Net_Set(OPCODE_VELOCITY_CONTROL, ccst_real_velocity, Protocol.Sizes[OPCODE_VELOCITY_CONTROL]);
	}
	ccst_real_velocity = NULL;

	if (ccst_real_setBlockPerm && Protocol.Handlers[OPCODE_SET_BLOCK_PERMISSION] == CCST_Netshim_SetBlockPermission) {
		Net_Set(OPCODE_SET_BLOCK_PERMISSION, ccst_real_setBlockPerm, Protocol.Sizes[OPCODE_SET_BLOCK_PERMISSION]);
	}
	ccst_real_setBlockPerm = NULL;

	if (ccst_real_setHotbar && Protocol.Handlers[OPCODE_SET_HOTBAR] == CCST_Netshim_SetHotbar) {
		Net_Set(OPCODE_SET_HOTBAR, ccst_real_setHotbar, Protocol.Sizes[OPCODE_SET_HOTBAR]);
	}
	ccst_real_setHotbar = NULL;
}

void CCST_Netshim_Refresh(void) {
	if (Server.IsSinglePlayer || Server.Disconnected) return;
	CCST_Netshim_Remove();
	CCST_Netshim_Install();
}

static void CCST_Netshim_OnConnected(void* obj) {
	(void)obj;
	CCST_Netshim_Install();
}

static void CCST_Netshim_OnDisconnected(void* obj) {
	(void)obj;
	CCST_Netshim_Remove();
}

void CCST_Netshim_Init(void) {
	Event_Register_(&NetEvents.Connected, NULL, CCST_Netshim_OnConnected);
	Event_Register_(&NetEvents.Disconnected, NULL, CCST_Netshim_OnDisconnected);
	/* If already connected (e.g. plugin reload rare), try now. */
	if (!Server.IsSinglePlayer && !Server.Disconnected)
		CCST_Netshim_Install();
}

void CCST_Netshim_Free(void) {
	Event_Unregister_(&NetEvents.Connected, NULL, CCST_Netshim_OnConnected);
	Event_Unregister_(&NetEvents.Disconnected, NULL, CCST_Netshim_OnDisconnected);
	CCST_Netshim_Remove();
}
