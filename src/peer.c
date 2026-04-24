/*
    peer.c
    P2P combat and steganography.

    - manages attack-tag encoding for melee
    - death channel framed bit stream
    - lag-compensated hit validation
    - RTT tracking and ping hooks
*/
#include "PluginAPI.h"
#include "Constants.h"
#include "Entity.h"
#include "Event.h"
#include "Camera.h"
#include "ExtMath.h"
#include "Game.h"
#include "Input.h"
#include "Model.h"
#include "Physics.h"
#include "Platform.h"
#include "Protocol.h"
#include "Server.h"
#include "Vectors.h"
#include "World.h"
#include "String_.h"
#include "deathmsg.h"
#include "health.h"
#include "peer.h"
#include "policy.h"
#include <string.h>

#define CCST_PEER_SENDBUF           512
#define CCST_PEER_ATTACK_CONFIRM    2    
#define CCST_PEER_MELEE_COOLDOWN    10   
#define CCST_PEER_HIST_CAP          64   
#define CCST_PEER_ROLLBACK_PAD_MS   30   
#define CCST_PEER_ATTACK_INTENT_MS  40   
#define CCST_PEER_MIN_ATTACK_SIGNAL_SEC 0.35 
#define CCST_PEER_FRUSTUM_SLACK     1.08f 

#define CCST_PEER_HIT_AABB_PADDING   0.18f
#define CCST_PEER_HIT_REACH_LEEWAY   0.22f
#define CCST_PEER_HIT_LAG_AGE_SLACK  3    
#define CCST_PEER_PING_RING         10   

// death channel v4 constants
#define CCST_PEER_DEATH_MAGIC        0xB4u
#define CCST_PEER_DEATH_MAGIC_BITS   8
#define CCST_PEER_DEATH_CRC_BITS     4
#define CCST_PEER_DEATH_FIX_BITS     16
#define CCST_PEER_DEATH_MAX_BITS     24
#define CCST_PEER_DEATH_HOLD_FRAMES  7
#define CCST_PEER_DEATH_REPEATS      2
#define CCST_PEER_DEATH_DEDUP_TICKS  24

static int CCST_Peer_GetGatedMeleeTarget(void);
static cc_bool CCST_Peer_ShouldEncodeOutgoingAttack(void);
static cc_bool CCST_Peer_ShouldEncodeOutgoingDeath(void);
static int CCST_Peer_InciterBits(void);
static void CCST_Peer_DeathTxReset(void);
static void CCST_Peer_DeathRxResetAll(void);

// RTT tracking
struct CCST_PeerPingSlot {
	cc_int64 sent, recv;
	cc_uint16 id;
};
static struct CCST_PeerPingSlot s_pingSlots[CCST_PEER_PING_RING];
static int s_pingHeadIdx;
static Net_Handler s_realTwoWayPing;

static cc_uint16 CCST_Peer_GetU16BE(const cc_uint8* d) {
	return (cc_uint16)((cc_uint16)d[0] << 8 | (cc_uint16)d[1]);
}

static void CCST_Peer_PingReset(void) {
	memset(s_pingSlots, 0, sizeof(s_pingSlots));
	s_pingHeadIdx = 0;
}

static void CCST_Peer_PingNoteOutgoing(cc_uint16 id) {
	int i;
	int head;

	for (i = 0; i < CCST_PEER_PING_RING; i++) {
		if (s_pingSlots[i].id == id && s_pingSlots[i].sent && !s_pingSlots[i].recv) return;
	}

	head = s_pingHeadIdx;
	head = (head + 1) % CCST_PEER_PING_RING;
	s_pingSlots[head].id   = id;
	s_pingSlots[head].sent = Stopwatch_Measure();
	s_pingSlots[head].recv = 0;
	s_pingHeadIdx = head;
}

static void CCST_Peer_PingOnResponse(int id) {
	cc_uint16 uid = (cc_uint16)id;
	int i;

	for (i = 0; i < CCST_PEER_PING_RING; i++) {
		if (s_pingSlots[i].id != uid) continue;
		if (!s_pingSlots[i].sent || s_pingSlots[i].recv) continue;
		s_pingSlots[i].recv = Stopwatch_Measure();
		return;
	}
}

static int CCST_Peer_AverageRttMs(void) {
	int i, n = 0;
	cc_uint64 sumUs = 0;

	for (i = 0; i < CCST_PEER_PING_RING; i++) {
		if (!s_pingSlots[i].sent || !s_pingSlots[i].recv) continue;
		sumUs += Stopwatch_ElapsedMicroseconds(s_pingSlots[i].sent, s_pingSlots[i].recv);
		n++;
	}
	if (!n) return 0;
	return (int)((sumUs / (cc_uint64)n) / 1000ULL);
}

static void CCST_Peer_NoteOutgoingTwoWayPingsInBuffer(const cc_uint8* buf, cc_uint32 len) {
	cc_uint32 pos = 0;

	while (pos < len) {
		cc_uint8 op = buf[pos];
		cc_uint32 sz;

		if (op >= OPCODE_COUNT) return;
		sz = Protocol.Sizes[op];
		if (sz == 0 || pos + sz > len) return;

		if (op == OPCODE_TWO_WAY_PING && sz >= 4 && buf[pos + 1] == 0) {
			CCST_Peer_PingNoteOutgoing(CCST_Peer_GetU16BE(buf + pos + 2));
		}
		pos += sz;
	}
}

static void CCST_Peer_TwoWayPingHook(cc_uint8* data) {
	if (data && !data[0]) {
		CCST_Peer_PingOnResponse((int)CCST_Peer_GetU16BE(data + 1));
	}
	if (s_realTwoWayPing) s_realTwoWayPing(data);
}

static void CCST_Peer_InstallPingHook(void) {
	Net_Handler h;

	if (Server.IsSinglePlayer) return;
	h = Protocol.Handlers[OPCODE_TWO_WAY_PING];
	if (!h || h == CCST_Peer_TwoWayPingHook) return;
	s_realTwoWayPing = h;
	Net_Set(OPCODE_TWO_WAY_PING, CCST_Peer_TwoWayPingHook, Protocol.Sizes[OPCODE_TWO_WAY_PING]);
}

static void CCST_Peer_RemovePingHook(void) {
	if (s_realTwoWayPing && Protocol.Handlers[OPCODE_TWO_WAY_PING] == CCST_Peer_TwoWayPingHook) {
		Net_Set(OPCODE_TWO_WAY_PING, s_realTwoWayPing, Protocol.Sizes[OPCODE_TWO_WAY_PING]);
	}
	s_realTwoWayPing = NULL;
}

/* Entity_GetEyePosition is not CC_API, mirror Entity.c (Position + GetEyeY * ModelScale). */
static Vec3 CCST_Peer_EyePos(struct Entity* e) {
	Vec3 pos;
	if (!e) {
		Vec3_Set(pos, 0.0f, 0.0f, 0.0f);
		return pos;
	}
	pos = e->Position;
	if (e->Model && e->Model->GetEyeY) {
		pos.y += e->Model->GetEyeY(e) * e->ModelScale.y;
	} else {
		pos.y += e->Size.y * 0.95f;
	}
	return pos;
}

/* Vec3_GetDirVector is not CC_API, mirror Vectors.c. */
static Vec3 CCST_Peer_GetDirVector(float yawRad, float pitchRad) {
	float x = -Math_CosF(pitchRad) * -Math_SinF(yawRad);
	float y = -Math_SinF(pitchRad);
	float z = -Math_CosF(pitchRad) * Math_CosF(yawRad);
	return Vec3_Create3(x, y, z);
}

static void (*s_downstream)(const cc_uint8* data, cc_uint32 len);
static cc_bool s_wrapped;
static double s_meleeFloorUntil; /* After a gated mousedown, encode at least until this Game.Time */
static cc_uint8 s_attackSeq;     /* toggles yaw bit 1 each patch while encoding */
static cc_bool s_lmbHeld;        /* Input.Pressed is not CC_VAR, mirror via Down2/Up2 */

/* Death TX: pending bit stream (MSB-first), drained 2 bits per pair advance. Each pair is held on
 * the wire for HOLD_FRAMES outgoing net frames so the server's slower broadcast always samples it. */
static cc_uint8 s_deathTxBits[CCST_PEER_DEATH_MAX_BITS];
static int s_deathTxCount;
static int s_deathTxIdx;
static cc_uint8 s_deathTxCurPair;   /* current pair on the wire: bit1 = hi, bit0 = lo */
static cc_uint8 s_deathTxClock;     /* flips on every pair advance; written to yaw[2] */
static int s_deathTxHoldLeft;       /* outgoing frames remaining in current pair hold window */
static int s_deathTxRepeatsLeft;    /* full-stream repetitions still to transmit */
static int s_deathTxInciterBits;    /* N captured at SendDeath; stream is invalid if it changes */

/* ---------- incoming ---------- */

static Net_Handler s_realOri;
static Net_Handler s_realRelOri;
static Net_Handler s_realTeleport;

static cc_uint8 s_attackSig[ENTITIES_MAX_COUNT];
static int s_lastMeleeTick[ENTITIES_MAX_COUNT];

/* Death RX: per-entity rolling shift register (low bits = newest). */
static cc_uint64 s_deathRxBuf[ENTITIES_MAX_COUNT];
static cc_uint8  s_deathRxCount[ENTITIES_MAX_COUNT]; /* caps at MAX_BITS */
static cc_uint8  s_deathRxLastStegoYaw[ENTITIES_MAX_COUNT]; /* last yaw[2:0] appended; 0xFF = none */
static int       s_deathRxLastShownTick[ENTITIES_MAX_COUNT]; /* game tick of last CCST_DeathMsg_ShowFor */
static int       s_deathRxLastShownKey[ENTITIES_MAX_COUNT];  /* cause<<16 | inciter+1; narrow dedup only */

/* Monotonic game tick (20/s) for cooldown, incremented from scheduled task. */
static int s_gameTick;

/*
 * Per-tick pose ring for all entity slots (same s_histHead as victim used to have alone).
 * Hit validation rewinds attacker + victim by the same age so the ray and target body line up in time.
 */
static Vec3 s_histFoot[CCST_PEER_HIST_CAP][ENTITIES_MAX_COUNT];
static Vec3 s_histSize[CCST_PEER_HIST_CAP][ENTITIES_MAX_COUNT];
static float s_histYaw[CCST_PEER_HIST_CAP][ENTITIES_MAX_COUNT];
static float s_histPitch[CCST_PEER_HIST_CAP][ENTITIES_MAX_COUNT];
static cc_uint8 s_histPop[CCST_PEER_HIST_CAP][ENTITIES_MAX_COUNT];
static int s_histHead; /* next slot to write */
static int s_histCount;

static cc_uint32 CCST_Peer_OutgoingTeleportWire(void) {
	cc_uint32 sz = Protocol.Sizes[OPCODE_ENTITY_TELEPORT];
	cc_uint32 bf = Protocol.Sizes[OPCODE_SET_BLOCK] > 8 ? 2 : 1;
	cc_uint32 coordBytes = sz - 1 - bf - 2;

	if (coordBytes == 6 || coordBytes == 12) return sz;
	if (coordBytes + 1 == 6 || coordBytes + 1 == 12) return sz + 1;
	return 0;
}

static cc_bool CCST_Peer_IsAttackWire(cc_uint8 yaw, cc_uint8 pitch) {
	return (yaw & 0x01) != 0 && (pitch & 0x03) == 0;
}

static cc_bool CCST_Peer_IsDeathWire(cc_uint8 yaw, cc_uint8 pitch) {
	/* Fully disjoint from attack (which forces pitch[1:0] = 00); the receiver's magic + CRC
	 * supplies the real protection, so the tag only needs to be mutually exclusive. */
	(void)yaw;
	return (pitch & 0x03) == 0x03;
}

static void CCST_Peer_PatchOutgoingAttack(cc_uint8* pkt, cc_uint32 wire) {
	cc_uint8* yawPtr   = pkt + wire - 2;
	cc_uint8* pitchPtr = pkt + wire - 1;

	*yawPtr   = (cc_uint8)((*yawPtr & 0xFC) | ((s_attackSeq & 1) << 1) | 0x01);
	*pitchPtr = (cc_uint8)(*pitchPtr & 0xFC);
	s_attackSeq++;
}

static void CCST_Peer_PatchOutgoingDeath(cc_uint8* pkt, cc_uint32 wire) {
	cc_uint8* yawPtr   = pkt + wire - 2;
	cc_uint8* pitchPtr = pkt + wire - 1;
	cc_uint8 hi, lo;

	/* When hold expires, load the next pair or restart for a new repeat cycle.
	 * pair_clock flips on each pair advance so the wire byte always differs between windows
	 * even when two adjacent pairs encode the same 2 bits (server dedup would otherwise
	 * swallow the first broadcast of the new pair). */
	if (s_deathTxHoldLeft <= 0) {
		if (s_deathTxIdx + 1 >= s_deathTxCount && s_deathTxRepeatsLeft > 0) {
			s_deathTxIdx = 0;
			s_deathTxRepeatsLeft--;
		}
		if (s_deathTxIdx + 1 < s_deathTxCount) {
			cc_uint8 bitHi = s_deathTxBits[s_deathTxIdx++];
			cc_uint8 bitLo = s_deathTxBits[s_deathTxIdx++];
			s_deathTxCurPair  = (cc_uint8)(((bitHi & 1) << 1) | (bitLo & 1));
			s_deathTxClock   ^= 1;
			s_deathTxHoldLeft = CCST_PEER_DEATH_HOLD_FRAMES;
		}
	}
	if (s_deathTxHoldLeft <= 0) return;

	hi = (cc_uint8)((s_deathTxCurPair >> 1) & 1u);
	lo = (cc_uint8)(s_deathTxCurPair & 1u);

	*yawPtr   = (cc_uint8)((*yawPtr   & 0xF8) | ((s_deathTxClock & 1) << 2) | (hi << 1) | lo);
	*pitchPtr = (cc_uint8)((*pitchPtr & 0xFC) | 0x03);

	s_deathTxHoldLeft--;
}

/* ---------- canonical entity indexing (both clients agree via sorted names) ---------- */

/*
 * Return 1 if entity slot has a usable display name (TabList entry or Entity.NameRaw).
 * Writes the name into out (capacity STRING_SIZE).
 */
static cc_bool CCST_Peer_GetEntityName(int id, cc_string* out) {
	struct Entity* e;
	cc_string raw;

	if (id < 0 || id >= ENTITIES_MAX_COUNT) return false;
	if (TabList.NameOffsets[id]) {
		raw = TabList_UNSAFE_GetPlayer(id);
		String_AppendColorless(out, &raw);
		return out->length > 0;
	}
	e = Entities.List[id];
	if (e && e->NameRaw[0]) {
		int len = String_CalcLen(e->NameRaw, STRING_SIZE);
		raw = String_Init(e->NameRaw, len, STRING_SIZE);
		String_AppendColorless(out, &raw);
		return out->length > 0;
	}
	return false;
}

static int CCST_Peer_CompareEntityNames(int a, int b) {
	cc_string na, nb;
	char bufA[STRING_SIZE], bufB[STRING_SIZE];
	int cmp;

	String_InitArray(na, bufA);
	String_InitArray(nb, bufB);
	CCST_Peer_GetEntityName(a, &na);
	CCST_Peer_GetEntityName(b, &nb);

	cmp = String_Compare(&na, &nb);
	if (cmp != 0) return cmp;
	/* Tie-break by entity id so every client agrees when two slots share a name. */
	return (a < b) ? -1 : (a > b ? 1 : 0);
}

/* O(n^2) but n ≤ 256; only runs on death emit/decode. Fixed 8-bit canonical index. */
static int CCST_Peer_EntityIdToCanonical(int id) {
	int i, rank = 0;
	cc_string tmp; char buf[STRING_SIZE];

	String_InitArray(tmp, buf);
	if (!CCST_Peer_GetEntityName(id, &tmp)) return -1;

	for (i = 0; i < ENTITIES_MAX_COUNT; i++) {
		if (i == id) continue;
		String_InitArray(tmp, buf);
		if (!CCST_Peer_GetEntityName(i, &tmp)) continue;
		if (CCST_Peer_CompareEntityNames(i, id) < 0) rank++;
	}
	return rank;
}

static int CCST_Peer_CanonicalToEntityId(int idx) {
	int i, rank;
	cc_string tmp; char buf[STRING_SIZE];

	for (i = 0; i < ENTITIES_MAX_COUNT; i++) {
		String_InitArray(tmp, buf);
		if (!CCST_Peer_GetEntityName(i, &tmp)) continue;
		rank = CCST_Peer_EntityIdToCanonical(i);
		if (rank == idx) return i;
	}
	return -1;
}

/* ---------- CRC-4/ITU-T (poly x^4+x+1 = 0x03, init 0, MSB-first) ---------- */

static cc_uint8 CCST_Peer_Crc4(cc_uint32 data, int nbits) {
	cc_uint8 crc = 0;
	int i;
	for (i = nbits - 1; i >= 0; i--) {
		cc_uint8 bit = (cc_uint8)((data >> i) & 1u);
		if (((crc >> 3) & 1u) ^ bit) crc = (cc_uint8)((crc << 1) ^ 0x03u);
		else                          crc = (cc_uint8)(crc << 1);
		crc &= 0x0Fu;
	}
	return crc;
}

/*
 * Minimum even number of bits required to index all currently-named entities (TabList or NameRaw).
 * "Even" keeps total_bits (FIX_BITS=16 + inciter_bits) even so every pair boundary aligns.
 * Both sender and receiver call this independently; they agree because entity lists are server-sync.
 */
static int CCST_Peer_InciterBits(void) {
	int count = 0, bits = 0, i;
	cc_string tmp; char buf[STRING_SIZE];
	for (i = 0; i < ENTITIES_MAX_COUNT; i++) {
		String_InitArray(tmp, buf);
		if (CCST_Peer_GetEntityName(i, &tmp)) count++;
	}
	while ((1 << bits) < count) bits++;
	if (bits & 1) bits++; /* round up to keep total bits even */
	return bits;
}

/* ---------- death channel TX ---------- */

static void CCST_Peer_DeathTxReset(void) {
	s_deathTxCount        = 0;
	s_deathTxIdx          = 0;
	s_deathTxCurPair      = 0;
	s_deathTxClock        = 0;
	s_deathTxHoldLeft     = 0;
	s_deathTxRepeatsLeft  = 0;
	s_deathTxInciterBits  = 0;
}

/* Clear every entity's rolling death shift register and dedup state. Used on connect,
 * disconnect, level change, and any roster edge where stale bits could cross-contaminate
 * the decoder (e.g. previous slot occupant's partial stream, or stale canonical indices). */
static void CCST_Peer_DeathRxResetAll(void) {
	int i;
	for (i = 0; i < ENTITIES_MAX_COUNT; i++) {
		s_deathRxBuf[i]           = 0;
		s_deathRxCount[i]         = 0;
		s_deathRxLastStegoYaw[i]  = 0xFF;
		s_deathRxLastShownTick[i] = -1000000;
		s_deathRxLastShownKey[i]  = 0;
	}
}

static cc_bool CCST_Peer_ShouldEncodeOutgoingDeath(void) {
	if (!CCST_Policy_PluginEnabled()) return false;
	if (!CCST_Policy_DeathMsgStreamEnabled()) return false;
	if (Server.IsSinglePlayer)         return false;
	/* Encode while bits remain to load OR the current pair is still mid-hold. */
	return s_deathTxIdx < s_deathTxCount || s_deathTxHoldLeft > 0 || s_deathTxRepeatsLeft > 0;
}

static void CCST_Peer_DeathPushBitsMsb(cc_uint32 value, int nbits) {
	int i;
	for (i = nbits - 1; i >= 0; i--) {
		if (s_deathTxCount >= CCST_PEER_DEATH_MAX_BITS) return;
		s_deathTxBits[s_deathTxCount++] = (cc_uint8)((value >> i) & 1u);
	}
}

void CCST_Peer_SendDeath(int cause, int inciterEntityId) {
	int has_inciter, idx, inciter_bits, payload_bits;
	cc_uint32 payload;
	cc_uint8 crc;

	if (Server.IsSinglePlayer)        return;
	if (!CCST_Policy_PluginEnabled()) return;

	cause &= 0x07;

	inciter_bits = CCST_Peer_InciterBits();
	has_inciter  = (inciterEntityId >= 0 && inciter_bits > 0) ? 1 : 0;
	idx = 0;
	if (has_inciter) {
		idx = CCST_Peer_EntityIdToCanonical(inciterEntityId);
		if (idx < 0 || idx >= (1 << inciter_bits)) {
			has_inciter = 0;
			idx = 0;
		}
	}

	/* payload = cause(3) | has_inciter(1) | inciter_idx(inciter_bits) */
	payload_bits = 3 + 1 + inciter_bits;
	payload = ((cc_uint32)cause       << (1 + inciter_bits))
	        | ((cc_uint32)has_inciter << inciter_bits)
	        | ((inciter_bits > 0) ? ((cc_uint32)idx & ((1u << inciter_bits) - 1u)) : 0u);
	crc = CCST_Peer_Crc4(payload, payload_bits);

	CCST_Peer_DeathTxReset();
	CCST_Peer_DeathPushBitsMsb(CCST_PEER_DEATH_MAGIC, CCST_PEER_DEATH_MAGIC_BITS);
	CCST_Peer_DeathPushBitsMsb(payload,               payload_bits);
	CCST_Peer_DeathPushBitsMsb(crc,                   CCST_PEER_DEATH_CRC_BITS);
	s_deathTxRepeatsLeft = CCST_PEER_DEATH_REPEATS - 1;
	s_deathTxInciterBits = inciter_bits;
}

static cc_bool CCST_Peer_PatchBuffer(cc_uint8* buf, cc_uint32 len) {
	cc_uint32 pos = 0;
	cc_uint32 teleWire = CCST_Peer_OutgoingTeleportWire();

	while (pos < len) {
		cc_uint8 op = buf[pos];
		cc_uint32 sz;

		if (op >= OPCODE_COUNT) return false;
		sz = Protocol.Sizes[op];
		if (sz == 0 || pos + sz > len) return false;

		if (op == OPCODE_ENTITY_TELEPORT && teleWire != 0) {
			if (pos + teleWire > len) return false;
			/* Death stream has priority as long as bits remain, hold is active, or a repeat is
			 * pending. Without checking holdLeft/repeatsLeft here, PatchOutgoingAttack would
			 * overwrite the final pair with pitch[1:0]=00, leaving the receiver 2 bits short. */
			if (s_deathTxIdx < s_deathTxCount || s_deathTxHoldLeft > 0 || s_deathTxRepeatsLeft > 0) {
				CCST_Peer_PatchOutgoingDeath(buf + pos, teleWire);
			} else {
				CCST_Peer_PatchOutgoingAttack(buf + pos, teleWire);
			}
			pos += teleWire;
			continue;
		}
		pos += sz;
	}
	return true;
}

static void CCST_Peer_SendDataShim(const cc_uint8* data, cc_uint32 len) {
	cc_uint8 local[CCST_PEER_SENDBUF];

	if (!s_downstream) return;
	if (!data || !len) {
		s_downstream(data, len);
		return;
	}
	/* Outgoing TWO_WAY_PING (CPE), correlate with responses for RTT (Ping_* in Server.c is not CC_API). */
	if (!Server.IsSinglePlayer)
		CCST_Peer_NoteOutgoingTwoWayPingsInBuffer(data, len);

	if (!CCST_Policy_PluginEnabled() || Server.IsSinglePlayer) {
		s_downstream(data, len);
		return;
	}
	/* Idle: no orientation stego, pass through unchanged. */
	if (!CCST_Peer_ShouldEncodeOutgoingDeath() && !CCST_Peer_ShouldEncodeOutgoingAttack()) {
		s_downstream(data, len);
		return;
	}
	if (len > sizeof(local)) {
		s_downstream(data, len);
		return;
	}

	memcpy(local, data, len);
	if (!CCST_Peer_PatchBuffer(local, len)) {
		s_downstream(data, len);
		return;
	}
	s_downstream(local, len);
}

/* ---------- rollback snapshot ---------- */

static void CCST_Peer_SnapshotEntities(void) {
	int slot, i;
	struct Entity* e;

	if (!World.Loaded) return;

	slot = s_histHead;
	for (i = 0; i < ENTITIES_MAX_COUNT; i++) {
		e = Entities.List[i];
		if (e) {
			s_histFoot[slot][i]  = e->Position;
			s_histSize[slot][i]  = e->Size;
			s_histYaw[slot][i]   = e->Yaw;
			s_histPitch[slot][i] = e->Pitch;
			s_histPop[slot][i]   = 1;
		} else {
			s_histPop[slot][i] = 0;
		}
	}

	s_histHead = (s_histHead + 1) % CCST_PEER_HIST_CAP;
	if (s_histCount < CCST_PEER_HIST_CAP) s_histCount++;
}

/* Historical pose for lag compensation; victim falls back to current AABB if ring empty. */
static cc_bool CCST_Peer_HistGetEntityRecord(int entityId, int ageTicks,
	Vec3* outFoot, Vec3* outSize, float* outYaw, float* outPitch) {
	int idx;
	struct Entity* live;

	if (entityId < 0 || entityId >= ENTITIES_MAX_COUNT) return false;
	if (!outFoot || !outSize) return false;

	if (s_histCount == 0) {
		live = Entities.List[entityId];
		if (!live) return false;
		*outFoot  = live->Position;
		*outSize  = live->Size;
		if (outYaw)   *outYaw   = live->Yaw;
		if (outPitch) *outPitch = live->Pitch;
		return true;
	}
	if (ageTicks < 0) ageTicks = 0;
	if (ageTicks > s_histCount - 1) ageTicks = s_histCount - 1;

	idx = (s_histHead - 1 - ageTicks + CCST_PEER_HIST_CAP * 2) % CCST_PEER_HIST_CAP;
	if (!s_histPop[idx][entityId]) {
		/* Self: keep old behaviour (always testable). Others: no ghost pose. */
		if (entityId == ENTITIES_SELF_ID) {
			live = Entities.List[ENTITIES_SELF_ID];
			if (!live) return false;
			*outFoot  = live->Position;
			*outSize  = live->Size;
			if (outYaw)   *outYaw   = live->Yaw;
			if (outPitch) *outPitch = live->Pitch;
			return true;
		}
		return false;
	}
	*outFoot  = s_histFoot[idx][entityId];
	*outSize  = s_histSize[idx][entityId];
	if (outYaw)   *outYaw   = s_histYaw[idx][entityId];
	if (outPitch) *outPitch = s_histPitch[idx][entityId];
	return true;
}

static void CCST_Peer_HistGetAABB(int ageTicks, struct AABB* out) {
	struct LocalPlayer* p;
	Vec3 foot, sz;
	float yaw, pitch;

	p = Entities.CurPlayer;
	if (!p || !out) return;

	if (!CCST_Peer_HistGetEntityRecord(ENTITIES_SELF_ID, ageTicks, &foot, &sz, &yaw, &pitch)) {
		AABB_Make(out, &p->Base.Position, &p->Base.Size);
		return;
	}
	AABB_Make(out, &foot, &sz);
}

/* ---------- ray vs AABB (axis-aligned collision bounds) ---------- */

/*
 * ClassiCube uses axis-aligned AABBs from Entity.Position + Entity.Size for **walking/block collision**.
 * `Intersection_RayIntersectsRotatedBox` / picking use **model** bounds plus RotY/RotX/RotZ for crosshair
 * selection, that path is not CC_API. For P2P melee we use the same axis-aligned **collision** box as
 * physics so hit tests match “body in the world,” and we inline the slab test so plugins do not link
 * Physics intersection symbols (see ClassiCube/src/Physics.c Intersection_RayIntersectsBox).
 */
static cc_bool CCST_Peer_RayIntersectsSlabAABB(Vec3 origin, Vec3 invDir, Vec3 min, Vec3 max, float* t0, float* t1) {
	float tmin, tmax, tymin, tymax, tzmin, tzmax;

	*t0 = 0.0f;
	*t1 = 0.0f;

	if (invDir.x >= 0.0f) {
		tmin = (min.x - origin.x) * invDir.x;
		tmax = (max.x - origin.x) * invDir.x;
	} else {
		tmin = (max.x - origin.x) * invDir.x;
		tmax = (min.x - origin.x) * invDir.x;
	}

	if (invDir.y >= 0.0f) {
		tymin = (min.y - origin.y) * invDir.y;
		tymax = (max.y - origin.y) * invDir.y;
	} else {
		tymin = (max.y - origin.y) * invDir.y;
		tymax = (min.y - origin.y) * invDir.y;
	}

	if (tmin > tymax || tymin > tmax) return false;
	if (tymin > tmin) tmin = tymin;
	if (tymax < tmax) tmax = tymax;

	if (invDir.z >= 0.0f) {
		tzmin = (min.z - origin.z) * invDir.z;
		tzmax = (max.z - origin.z) * invDir.z;
	} else {
		tzmin = (max.z - origin.z) * invDir.z;
		tzmax = (min.z - origin.z) * invDir.z;
	}

	if (tmin > tzmax || tzmin > tmax) return false;
	if (tzmin > tmin) tmin = tzmin;
	if (tzmax < tmax) tmax = tzmax;

	*t0 = tmin;
	*t1 = tmax;
	return tmin >= 0.0f;
}

static cc_bool CCST_Peer_RayHitsAABB(Vec3 origin, Vec3 dir, const struct AABB* bb, float reach) {
	Vec3 invDir;
	float t0, t1;

	invDir.x = Math_SafeDiv(1.0f, dir.x);
	invDir.y = Math_SafeDiv(1.0f, dir.y);
	invDir.z = Math_SafeDiv(1.0f, dir.z);

	if (!CCST_Peer_RayIntersectsSlabAABB(origin, invDir, bb->Min, bb->Max, &t0, &t1)) return false;
	if (t0 < 0.0f) t0 = t1;
	return t0 >= 0.0f && t0 <= reach;
}

static void CCST_Peer_AabbInflate(struct AABB* bb, float pad) {
	if (!bb || pad <= 0.0f) return;
	bb->Min.x -= pad;
	bb->Min.y -= pad;
	bb->Min.z -= pad;
	bb->Max.x += pad;
	bb->Max.y += pad;
	bb->Max.z += pad;
}

/* Closest player along look ray within block reach & view cone (Camera.Fov ~ horizontal; cone is a fair approximation). */
static cc_bool CCST_Peer_AabbCenterInFrustum(Vec3 eye, Vec3 forwardUnit, const struct AABB* bb, float cosHalfFov) {
	float cx, cy, cz, dx, dy, dz, dist, dot;

	cx = (bb->Min.x + bb->Max.x) * 0.5f;
	cy = (bb->Min.y + bb->Max.y) * 0.5f;
	cz = (bb->Min.z + bb->Max.z) * 0.5f;
	dx = cx - eye.x;
	dy = cy - eye.y;
	dz = cz - eye.z;
	dist = Math_SqrtF(dx * dx + dy * dy + dz * dz);
	if (dist < 1e-3f) return true;
	dot = (dx * forwardUnit.x + dy * forwardUnit.y + dz * forwardUnit.z) / dist;
	return dot >= cosHalfFov;
}

static int CCST_Peer_GetGatedMeleeTarget(void) {
	struct LocalPlayer* lp;
	Vec3 eye, dir;
	float t0, t1, reach;
	int best;
	float bestT;
	float cosHalfFov;
	int fovDeg;
	int i;

	lp = Entities.CurPlayer;
	if (!lp || !World.Loaded) return -1;

	reach = lp->ReachDistance;
	if (reach < 1e-3f) return -1;

	eye = CCST_Peer_EyePos(&lp->Base);
	dir = CCST_Peer_GetDirVector(lp->Base.Yaw * MATH_DEG2RAD, lp->Base.Pitch * MATH_DEG2RAD);

	fovDeg = Camera.Fov;
	if (fovDeg < 30) fovDeg = 70;
	cosHalfFov = Math_CosF((float)fovDeg * 0.5f * MATH_DEG2RAD * CCST_PEER_FRUSTUM_SLACK);

	best  = -1;
	bestT = 1e9f;

	for (i = 0; i < ENTITIES_SELF_ID; i++) {
		struct Entity* e = Entities.List[i];
		struct AABB entBB;
		Vec3 invDir;

		if (!e) continue;

		AABB_Make(&entBB, &e->Position, &e->Size);
		if (!CCST_Peer_AabbCenterInFrustum(eye, dir, &entBB, cosHalfFov)) continue;

		invDir.x = Math_SafeDiv(1.0f, dir.x);
		invDir.y = Math_SafeDiv(1.0f, dir.y);
		invDir.z = Math_SafeDiv(1.0f, dir.z);

		if (!CCST_Peer_RayIntersectsSlabAABB(eye, invDir, entBB.Min, entBB.Max, &t0, &t1)) continue;
		if (t0 < 0.0f) t0 = t1;
		if (t0 < 0.0f || t0 > reach) continue;
		if (best < 0 || t0 < bestT) {
			bestT = t0;
			best  = i;
		}
	}
	return best;
}

static cc_bool CCST_Peer_ShouldEncodeOutgoingAttack(void) {
	int tgt;

	if (!CCST_Policy_PluginEnabled()) return false;
	if (!CCST_Policy_CombatEnabled()) return false;
	if (!CCST_Health_IsSurvivalActive()) return false;
	if (CCST_Health_IsDeadOrDying()) return false;
	if (!Entities.CurPlayer || !World.Loaded) return false;

	tgt = CCST_Peer_GetGatedMeleeTarget();
	if (tgt < 0) return false;

	if (s_lmbHeld) return true;
	if (s_meleeFloorUntil > 0.0 && Game.Time < s_meleeFloorUntil) return true;
	return false;
}

/* ---------- hit test ---------- */

/*
 * Ticks to rewind both attacker aim and victim body: ~path from attacker's swing to this client
 * (RTT over relay), plus intent->wire delay and jitter. Same age for both sides.
 */
static int CCST_Peer_HitLagAgeTicks(void) {
	int rtt = CCST_Peer_AverageRttMs();
	int ms;

	if (rtt < 1) rtt = 80;
	ms = rtt + CCST_PEER_ROLLBACK_PAD_MS + CCST_PEER_ATTACK_INTENT_MS;
	if (ms > 500) ms = 500;
	return (ms * 20 + 999) / 1000;
}

/*
 * Single candidate age: historical attacker ray vs historical victim box (+ padding / reach leeway).
 * Returns whether the geometry test passes (does not apply damage).
 */
static cc_bool CCST_Peer_HitGeometryAtAge(int attackerId, struct Entity* attacker, float reach, int ageTicks) {
	Vec3 eye, dir, aFoot, aSize;
	struct AABB pastBB;
	float aYaw, aPitch;

	if (!CCST_Peer_HistGetEntityRecord(attackerId, ageTicks, &aFoot, &aSize, &aYaw, &aPitch))
		return false;
	CCST_Peer_HistGetAABB(ageTicks, &pastBB);
	CCST_Peer_AabbInflate(&pastBB, CCST_PEER_HIT_AABB_PADDING);

	eye = aFoot;
	if (attacker->Model && attacker->Model->GetEyeY) {
		eye.y += attacker->Model->GetEyeY(attacker) * attacker->ModelScale.y;
	} else {
		eye.y += aSize.y * 0.95f;
	}
	dir = CCST_Peer_GetDirVector(aYaw * MATH_DEG2RAD, aPitch * MATH_DEG2RAD);

	return CCST_Peer_RayHitsAABB(eye, dir, &pastBB, reach + CCST_PEER_HIT_REACH_LEEWAY);
}

static cc_bool CCST_Peer_TryApplyHit(int attackerId) {
	struct LocalPlayer* lp;
	struct Entity* attacker;
	int baseAge;
	int agesToTry[1 + 2 * CCST_PEER_HIT_LAG_AGE_SLACK];
	int nAges, i, s;
	float reach;

	if (attackerId < 0 || attackerId >= ENTITIES_SELF_ID) return false;
	if (!CCST_Health_IsSurvivalActive()) return false;
	if (CCST_Health_IsDeadOrDying()) return false;

	lp = Entities.CurPlayer;
	if (!lp) return false;
	/* Victim-side: use this client's reach (server/CPE; usually same for everyone), not attacker state. */
	reach = lp->ReachDistance;
	if (reach < 0.25f) reach = 5.0f; /* Entity.c default if unset */

	if (s_gameTick - s_lastMeleeTick[attackerId] < CCST_PEER_MELEE_COOLDOWN) return false;

	attacker = Entities.List[attackerId];
	if (!attacker) return false;

	baseAge = CCST_Peer_HitLagAgeTicks();
	nAges = 0;
	agesToTry[nAges++] = baseAge;
	for (s = 1; s <= CCST_PEER_HIT_LAG_AGE_SLACK; s++) {
		if (baseAge - s >= 0) agesToTry[nAges++] = baseAge - s;
		agesToTry[nAges++] = baseAge + s;
	}

	for (i = 0; i < nAges; i++) {
		if (CCST_Peer_HitGeometryAtAge(attackerId, attacker, reach, agesToTry[i])) {
			CCST_Health_ApplyPeerMeleeHit(attackerId);
			s_lastMeleeTick[attackerId] = s_gameTick;
			return true;
		}
	}
	return false;
}

static void CCST_Peer_OnAttackWire(int id, cc_uint8 yawByte, cc_uint8 pitchByte) {
	if (id < 0 || id >= ENTITIES_MAX_COUNT) return;
	if (id == ENTITIES_SELF_ID) return;
	if (!CCST_Policy_CombatEnabled()) return;

	if (CCST_Peer_IsAttackWire(yawByte, pitchByte)) {
		if (s_attackSig[id] < 255) s_attackSig[id]++;
		if (s_attackSig[id] >= CCST_PEER_ATTACK_CONFIRM) {
			/* Continuous stream: try hit each frame while signal holds; invuln in health gates damage. */
			(void)CCST_Peer_TryApplyHit(id);
		}
	} else {
		s_attackSig[id] = 0;
	}
}

/*
 * Rolling-window decoder: append 2 bits per tagged frame (yaw[1:0]); once we have accumulated
 * at least total_bits = 16 + CCST_Peer_InciterBits() bits, check the window for magic + CRC4.
 * Append only yaw[1:0] to the register; use yaw[2:0] to dedupe duplicate server forwards while the
 * victim turns (high yaw bits change but stego low bits are unchanged).
 */
static void CCST_Peer_OnDeathWire(int id, cc_uint8 yawByte, cc_uint8 pitchByte) {
	cc_uint64 buf, window;
	cc_uint8 hi, lo, stegoYaw;
	int inciter_bits, total_bits, payload_bits;
	cc_uint32 magic_got, payload, crc_got;
	cc_uint8 crc_expected;
	int cause, has_inciter, inc_idx, inciterId, key;

	if (id < 0 || id >= ENTITIES_MAX_COUNT) return;
	if (id == ENTITIES_SELF_ID) return;
	if (!CCST_Peer_IsDeathWire(yawByte, pitchByte)) return;
	if (!CCST_Policy_DeathMsgStreamEnabled()) return;

	/* Ignore duplicate forwards: same clock+pair (yaw[2:0]) while upper bits moved with look. */
	stegoYaw = (cc_uint8)(yawByte & 0x07);
	if (stegoYaw == s_deathRxLastStegoYaw[id]) return;
	s_deathRxLastStegoYaw[id] = stegoYaw;

	/* Payload bits: yaw[1:0], MSB first. yaw[2] distinguishes adjacent pairs with identical pair bits. */
	hi = (cc_uint8)((yawByte >> 1) & 1u);
	lo = (cc_uint8)(yawByte & 1u);

	buf = s_deathRxBuf[id];
	buf = (buf << 2) | ((cc_uint64)hi << 1) | (cc_uint64)lo;
	s_deathRxBuf[id] = buf;

	if (s_deathRxCount[id] < CCST_PEER_DEATH_MAX_BITS) {
		int next = (int)s_deathRxCount[id] + 2;
		if (next > CCST_PEER_DEATH_MAX_BITS) next = CCST_PEER_DEATH_MAX_BITS;
		s_deathRxCount[id] = (cc_uint8)next;
	}

	inciter_bits = CCST_Peer_InciterBits();
	total_bits   = CCST_PEER_DEATH_FIX_BITS + inciter_bits; /* always even */
	payload_bits = 3 + 1 + inciter_bits;

	if (s_deathRxCount[id] < total_bits) return;

	/* Extract the most-recent total_bits window (low bits = newest). */
	window = buf & (((cc_uint64)1 << total_bits) - 1);

	/* Layout oldest->newest: [magic:8 | cause:3 | has_inciter:1 | inciter:N | crc4:4] */
	magic_got = (cc_uint32)((window >> (payload_bits + CCST_PEER_DEATH_CRC_BITS)) & 0xFFu);
	if (magic_got != CCST_PEER_DEATH_MAGIC) return;

	payload  = (cc_uint32)((window >> CCST_PEER_DEATH_CRC_BITS) & ((1u << payload_bits) - 1u));
	crc_got  = (cc_uint32)(window & 0x0Fu);

	crc_expected = CCST_Peer_Crc4(payload, payload_bits);
	if (crc_expected != (cc_uint8)crc_got) return;

	cause       = (int)((payload >> (1 + inciter_bits)) & 0x07u);
	has_inciter = (int)((payload >> inciter_bits) & 0x01u);
	inc_idx     = (inciter_bits > 0) ? (int)(payload & ((1u << inciter_bits) - 1u)) : 0;
	inciterId   = has_inciter ? CCST_Peer_CanonicalToEntityId(inc_idx) : -1;

	key = ((cause & 0xFF) << 16) | ((inciterId + 1) & 0xFFFF);
	/* Narrow dedup: same decode twice in a few ticks (rolling buffer tail). Do not use for
	 * "same death minutes apart" -- lastShownTick is only updated when we actually show. */
	if (s_deathRxLastShownKey[id] == key &&
	    s_gameTick - s_deathRxLastShownTick[id] < CCST_PEER_DEATH_DEDUP_TICKS) {
		s_deathRxBuf[id]          = 0;
		s_deathRxCount[id]        = 0;
		s_deathRxLastStegoYaw[id] = 0xFF;
		return;
	}

	s_deathRxLastShownKey[id]  = key;
	s_deathRxLastShownTick[id] = s_gameTick;

	CCST_DeathMsg_ShowFor(id, (CCST_DeathCause)cause, inciterId);

	s_deathRxBuf[id]          = 0;
	s_deathRxCount[id]        = 0;
	s_deathRxLastStegoYaw[id] = 0xFF;
}

static void CCST_Peer_OnPeerWire(int id, cc_uint8 yawByte, cc_uint8 pitchByte) {
	if (CCST_Peer_IsDeathWire(yawByte, pitchByte)) {
		CCST_Peer_OnDeathWire(id, yawByte, pitchByte);
		/* New tag is disjoint from attack (pitch[1:0]=11 vs 00), so attack sig cannot advance
		 * on this frame. Explicit reset keeps the sig state tidy when a peer switches channels. */
		if (id >= 0 && id < ENTITIES_MAX_COUNT) s_attackSig[id] = 0;
		return;
	}
	CCST_Peer_OnAttackWire(id, yawByte, pitchByte);
}

/* ---------- mousedown: minimum tap length ---------- */

static void CCST_Peer_OnInputUp(void* obj, int key, cc_bool repeating, struct InputDevice* device) {
	(void)obj;
	(void)repeating;
	(void)device;
	if (key == CCMOUSE_L) s_lmbHeld = false;
}

static void CCST_Peer_OnInputDown(void* obj, int key, cc_bool repeating, struct InputDevice* device) {
	int tgt;
	(void)obj;
	(void)device;

	if (key == CCMOUSE_L) s_lmbHeld = true;

	if (repeating) return;
	if (key != CCMOUSE_L) return;
	if (!CCST_Policy_PluginEnabled()) return;
	if (!CCST_Health_IsSurvivalActive()) return;
	if (CCST_Health_IsDeadOrDying()) return;

	tgt = CCST_Peer_GetGatedMeleeTarget();
	if (tgt < 0) return;

	s_meleeFloorUntil = Game.Time + CCST_PEER_MIN_ATTACK_SIGNAL_SEC;
}

static void CCST_Peer_OnGameTick(struct ScheduledTask* task) {
	(void)task;
	s_gameTick++;
	CCST_Peer_SnapshotEntities();
}

/* ---------- handler shims: sample wire bytes BEFORE stock handler mutates/interpolates ---------- */

static void CCST_Peer_OriShim(cc_uint8* data) {
	cc_uint8 y = data[1], p = data[2];
	int id = data[0];
	if (s_realOri) s_realOri(data);
	if (CCST_Policy_PluginEnabled()) CCST_Peer_OnPeerWire(id, y, p);
}

static void CCST_Peer_RelOriShim(cc_uint8* data) {
	cc_uint8 y = data[4], p = data[5];
	int id = data[0];
	if (s_realRelOri) s_realRelOri(data);
	if (CCST_Policy_PluginEnabled()) CCST_Peer_OnPeerWire(id, y, p);
}

static void CCST_Peer_TeleportShim(cc_uint8* data) {
	cc_uint32 payload = Protocol.Sizes[OPCODE_ENTITY_TELEPORT];
	cc_uint8 y, p;
	int id;

	if (payload < 2) {
		if (s_realTeleport) s_realTeleport(data);
		return;
	}
	payload -= 1;
	y = data[payload - 2];
	p = data[payload - 1];
	id = data[0];

	if (s_realTeleport) s_realTeleport(data);
	if (CCST_Policy_PluginEnabled()) CCST_Peer_OnPeerWire(id, y, p);
}

/* ---------- install / remove ---------- */

static void CCST_Peer_InstallSendShim(void) {
	if (s_wrapped) return;
	if (!Server.SendData) return;
	if (Server.IsSinglePlayer) return;
	s_downstream    = Server.SendData;
	Server.SendData = CCST_Peer_SendDataShim;
	s_wrapped       = true;
}

static void CCST_Peer_RemoveSendShim(void) {
	if (!s_wrapped) return;
	if (Server.SendData == CCST_Peer_SendDataShim) {
		Server.SendData = s_downstream;
	}
	s_downstream = NULL;
	s_wrapped    = false;
}

static void CCST_Peer_InstallRxShims(void) {
	Net_Handler h;

	h = Protocol.Handlers[OPCODE_ORI_UPDATE];
	if (h && h != CCST_Peer_OriShim) {
		s_realOri = h;
		Net_Set(OPCODE_ORI_UPDATE, CCST_Peer_OriShim,
			Protocol.Sizes[OPCODE_ORI_UPDATE]);
	}
	h = Protocol.Handlers[OPCODE_RELPOS_AND_ORI_UPDATE];
	if (h && h != CCST_Peer_RelOriShim) {
		s_realRelOri = h;
		Net_Set(OPCODE_RELPOS_AND_ORI_UPDATE, CCST_Peer_RelOriShim,
			Protocol.Sizes[OPCODE_RELPOS_AND_ORI_UPDATE]);
	}
	h = Protocol.Handlers[OPCODE_ENTITY_TELEPORT];
	if (h && h != CCST_Peer_TeleportShim) {
		s_realTeleport = h;
		Net_Set(OPCODE_ENTITY_TELEPORT, CCST_Peer_TeleportShim,
			Protocol.Sizes[OPCODE_ENTITY_TELEPORT]);
	}
}

static void CCST_Peer_RemoveRxShims(void) {
	if (s_realOri && Protocol.Handlers[OPCODE_ORI_UPDATE] == CCST_Peer_OriShim) {
		Net_Set(OPCODE_ORI_UPDATE, s_realOri, Protocol.Sizes[OPCODE_ORI_UPDATE]);
	}
	s_realOri = NULL;

	if (s_realRelOri && Protocol.Handlers[OPCODE_RELPOS_AND_ORI_UPDATE] == CCST_Peer_RelOriShim) {
		Net_Set(OPCODE_RELPOS_AND_ORI_UPDATE, s_realRelOri,
			Protocol.Sizes[OPCODE_RELPOS_AND_ORI_UPDATE]);
	}
	s_realRelOri = NULL;

	if (s_realTeleport && Protocol.Handlers[OPCODE_ENTITY_TELEPORT] == CCST_Peer_TeleportShim) {
		Net_Set(OPCODE_ENTITY_TELEPORT, s_realTeleport,
			Protocol.Sizes[OPCODE_ENTITY_TELEPORT]);
	}
	s_realTeleport = NULL;
}

/* ---------- event glue ---------- */

static void CCST_Peer_ResetState(void) {
	int i;

	memset(s_attackSig, 0, sizeof(s_attackSig));
	for (i = 0; i < ENTITIES_MAX_COUNT; i++) {
		s_lastMeleeTick[i]       = -1000000;
		s_deathRxBuf[i]          = 0;
		s_deathRxCount[i]        = 0;
		s_deathRxLastStegoYaw[i] = 0xFF;
		s_deathRxLastShownTick[i] = -1000000;
		s_deathRxLastShownKey[i]  = 0;
	}
	s_histHead    = 0;
	s_histCount   = 0;
	memset(s_histPop, 0, sizeof(s_histPop));
	s_gameTick    = 0;
	s_meleeFloorUntil = -1.0;
	s_attackSeq   = 0;
	s_lmbHeld     = false;
	CCST_Peer_DeathTxReset();
	CCST_Peer_PingReset();
}

static void CCST_Peer_OnConnected(void* obj) {
	(void)obj;
	CCST_Peer_ResetState();
	CCST_Peer_InstallSendShim();
	CCST_Peer_InstallRxShims();
	CCST_Peer_InstallPingHook();
}

static void CCST_Peer_OnDisconnected(void* obj) {
	(void)obj;
	CCST_Peer_RemovePingHook();
	CCST_Peer_RemoveRxShims();
	CCST_Peer_RemoveSendShim();
	CCST_Peer_ResetState();
}

/*
 * Roster edge: a new entity slot became populated. If we are mid-transmission, the canonical
 * inciter index width may no longer match what the receiver computes -- abort our stream; the
 * death event is gone, but correctness beats a silent misdecode. Also clear any stale partial
 * rx bits for this slot in case it was recycled from a prior occupant.
 */
static void CCST_Peer_OnEntityAdded(void* obj, int id) {
	(void)obj;
	if (id >= 0 && id < ENTITIES_MAX_COUNT) {
		s_attackSig[id]          = 0;
		s_deathRxBuf[id]         = 0;
		s_deathRxCount[id]       = 0;
		s_deathRxLastStegoYaw[id] = 0xFF;
		s_deathRxLastShownTick[id] = -1000000;
		s_deathRxLastShownKey[id]  = 0;
	}
	if (s_deathTxInciterBits != CCST_Peer_InciterBits()) {
		CCST_Peer_DeathTxReset();
	}
}

/*
 * New world starts loading: the entity roster on this client is being torn down and the lag-comp
 * history contains positions from the old map. Invalidate everything that depends on world/roster
 * identity. Server forwarding restarts from scratch after the map load; tab list and entities are
 * resent by the server, which fires Added events we reset on again.
 */
static void CCST_Peer_OnNewMap(void* obj) {
	int i;
	(void)obj;

	CCST_Peer_DeathTxReset();
	CCST_Peer_DeathRxResetAll();

	memset(s_attackSig, 0, sizeof(s_attackSig));
	for (i = 0; i < ENTITIES_MAX_COUNT; i++) s_lastMeleeTick[i] = -1000000;

	/* Lag-comp history: old-level poses must not be ray-tested against new-level entities. */
	s_histHead  = 0;
	s_histCount = 0;
	memset(s_histPop, 0, sizeof(s_histPop));
}

static void CCST_Peer_OnEntityRemoved(void* obj, int id) {
	(void)obj;
	if (id >= 0 && id < ENTITIES_MAX_COUNT) {
		s_attackSig[id]          = 0;
		s_lastMeleeTick[id]      = -1000000;
		s_deathRxBuf[id]         = 0;
		s_deathRxCount[id]       = 0;
		s_deathRxLastStegoYaw[id] = 0xFF;
		s_deathRxLastShownTick[id] = -1000000;
		s_deathRxLastShownKey[id]  = 0;
	}
	/* Removing a named entity may shrink N; abort any in-flight death tx so we do not finish
	 * sending a stream under a width the receiver no longer computes. */
	if (s_deathTxInciterBits != CCST_Peer_InciterBits()) {
		CCST_Peer_DeathTxReset();
	}
}

void CCST_Peer_Init(void) {
	CCST_Peer_ResetState();

	Event_Register_(&NetEvents.Connected,    NULL, CCST_Peer_OnConnected);
	Event_Register_(&NetEvents.Disconnected, NULL, CCST_Peer_OnDisconnected);
	Event_Register_(&EntityEvents.Added,     NULL, CCST_Peer_OnEntityAdded);
	Event_Register_(&EntityEvents.Removed,   NULL, CCST_Peer_OnEntityRemoved);
	Event_Register_(&WorldEvents.NewMap,     NULL, CCST_Peer_OnNewMap);
	Event_Register_(&InputEvents.Down2,      NULL, CCST_Peer_OnInputDown);
	Event_Register_(&InputEvents.Up2,        NULL, CCST_Peer_OnInputUp);

	ScheduledTask_Add(GAME_DEF_TICKS, CCST_Peer_OnGameTick);

	if (!Server.IsSinglePlayer && !Server.Disconnected) {
		CCST_Peer_InstallSendShim();
		CCST_Peer_InstallRxShims();
		CCST_Peer_InstallPingHook();
	}
}

void CCST_Peer_Free(void) {
	Event_Unregister_(&NetEvents.Connected,    NULL, CCST_Peer_OnConnected);
	Event_Unregister_(&NetEvents.Disconnected, NULL, CCST_Peer_OnDisconnected);
	Event_Unregister_(&EntityEvents.Added,     NULL, CCST_Peer_OnEntityAdded);
	Event_Unregister_(&EntityEvents.Removed,   NULL, CCST_Peer_OnEntityRemoved);
	Event_Unregister_(&WorldEvents.NewMap,     NULL, CCST_Peer_OnNewMap);
	Event_Unregister_(&InputEvents.Down2,      NULL, CCST_Peer_OnInputDown);
	Event_Unregister_(&InputEvents.Up2,        NULL, CCST_Peer_OnInputUp);
	CCST_Peer_RemovePingHook();
	CCST_Peer_RemoveRxShims();
	CCST_Peer_RemoveSendShim();
	CCST_Peer_ResetState();
}

void CCST_Peer_Refresh(void) {
	if (Server.IsSinglePlayer || Server.Disconnected) return;
	CCST_Peer_RemovePingHook();
	CCST_Peer_RemoveRxShims();
	CCST_Peer_InstallRxShims();
	CCST_Peer_InstallPingHook();
	CCST_Peer_InstallSendShim();
}
