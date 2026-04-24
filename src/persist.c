/*
    persist.c
    world-based state persistence.

    - spectral world descriptors for matching
    - position and gamemode restoration
    - autosave and manual restore commands
*/
#include "persist.h"
#include "ccst_api.h"
#include "fakeinventory.h"
#include "health.h"
#include "score.h"

#include "Block.h"
#include "Entity.h"
#include "Errors.h"
#include "Event.h"
#include "Platform.h"
#include "Server.h"
#include "Stream.h"
#include "String_.h"
#include "World.h"
#include "policy.h"

#include <math.h>
#include <float.h>
#include <string.h>

#if defined(_WIN32)
#define CCST_ALREADY_EXISTS ((cc_result)183)
#else
#include <errno.h>
#define CCST_ALREADY_EXISTS ((cc_result)EEXIST)
#endif

#define CCST_PERSIST_MAGIC 0x54535443u /* 'CCST' */
#define CCST_PERSIST_VER   5u

#define CCST_EMB_D 32

#define CCST_COARSE_MAX_X 24
#define CCST_COARSE_MAX_Y 24
#define CCST_COARSE_MAX_Z 24

#define CCST_LOW_KX 3
#define CCST_LOW_KY 3
#define CCST_LOW_KZ 3
#define CCST_LOW_BASIS (CCST_LOW_KX * CCST_LOW_KY * CCST_LOW_KZ)
#define CCST_LOW_LEN   (CCST_EMB_D * CCST_LOW_BASIS)

#define CCST_MID_KX 4
#define CCST_MID_KY 4
#define CCST_MID_KZ 4
#define CCST_MID_BASIS (CCST_MID_KX * CCST_MID_KY * CCST_MID_KZ)
#define CCST_MID_LEN   (CCST_EMB_D * CCST_MID_BASIS)

#define CCST_HIGH_KX 5
#define CCST_HIGH_KY 5
#define CCST_HIGH_KZ 5
#define CCST_HIGH_BASIS (CCST_HIGH_KX * CCST_HIGH_KY * CCST_HIGH_KZ)
#define CCST_HIGH_LEN   (CCST_EMB_D * CCST_HIGH_BASIS)

#define CCST_HIST_BINS 6

#define CCST_MATCH_W_LOW  0.40f
#define CCST_MATCH_W_MID  0.30f
#define CCST_MATCH_W_HIGH 0.20f
#define CCST_MATCH_W_HIST 0.10f

#define CCST_MATCH_THRESHOLD 0.50f
#define CCST_MATCH_MARGIN    0.02f
/* if best score is extremely low, accept even when second-best is also near (multiple snapshots of same world). */
#define CCST_MATCH_STRONG_ACCEPT 0.25f
/* "not really" bucket for user-assisted force restore prompt. */
#define CCST_MATCH_POSSIBLE_THRESHOLD 0.75f

/* save every 2 seconds */
#define CCST_SAVE_INTERVAL_TICKS 40
/* restore scheduling: wait a bit, then require a short stable window before matching once. */
#define CCST_RESTORE_MIN_DELAY_TICKS 20
#define CCST_RESTORE_MAX_WAIT_TICKS  240
#define CCST_RESTORE_STABLE_TICKS    12
#define CCST_RESTORE_SAMPLE_COUNT    1024

#define CCST_PERSIST_DEBUG 0

enum CCST_BlockClass {
	CCST_CLASS_AIR = 0,
	CCST_CLASS_SOLID,
	CCST_CLASS_LIQUID,
	CCST_CLASS_SPRITE,
	CCST_CLASS_GAS,
	CCST_CLASS_OTHER
};

struct CCST_WorldDescriptor {
	cc_uint16 coarseX, coarseY, coarseZ;
	float low[CCST_LOW_LEN];
	float mid[CCST_MID_LEN];
	float high[CCST_HIGH_LEN];
	float hist[CCST_HIST_BINS];
};

struct CCST_PersistFile {
	cc_uint32 magic;
	cc_uint16 version;
	cc_uint16 flags; /* bit0: position restore denied warning already shown, bit1: saved user gamemode survival */

	cc_uint16 worldW, worldH, worldL;
	cc_uint8 isSingleplayer;
	cc_uint8 _pad0[3];
	cc_uint32 serverPort;

	struct CCST_WorldDescriptor desc;

	float px, py, pz;
	float yaw, pitch;
	cc_int32 health;
	cc_int32 score;

	cc_uint16 blocks[CCST_INV_SLOTS];
	cc_uint8  counts[CCST_INV_SLOTS];
	cc_uint16 transmuteBlock;
	cc_uint8  transmuteCount;
	cc_uint8  _pad1;
};

static cc_bool ccst_persist_posDeniedWarned;
static int ccst_persist_saveAcc;
static cc_bool ccst_persist_restorePending;
static int ccst_persist_restoreDelayTicks;
static int ccst_persist_restoreTryTicks;
static int ccst_persist_restoreStableTicks;
static cc_uint64 ccst_persist_prevWorldFingerprint;
static cc_bool ccst_persist_prevFingerprintValid;
static cc_bool ccst_persist_restoreAttempted;
static cc_bool ccst_persist_savedNoMatch;
static cc_bool ccst_persist_hasActivePath;
static char ccst_persist_activePath[FILENAME_SIZE];
static cc_bool ccst_persist_hasSuggestedCandidate;
static struct CCST_PersistFile ccst_persist_suggestedCandidate;
static char ccst_persist_suggestedPath[FILENAME_SIZE];

static float ccst_persist_lastBestScore;
static float ccst_persist_lastSecondScore;
static int ccst_persist_debugLastStableLogged;
static int ccst_persist_debugTicksSinceLog;
static cc_bool ccst_opt_default_survival; /* false=creative, true=survival */
static cc_bool ccst_opt_autosave;
static cc_bool ccst_opt_firsttime_seen;

static cc_bool CCST_Persist_EnsureDir(const char* path);

static void CCST_Persist_Debug(const char* msg) {
#if CCST_PERSIST_DEBUG
	CCST_Chat(msg);
#else
	(void)msg;
#endif
}

enum CCST_PersistLoadFail {
	CCST_PLF_NONE = 0,
	CCST_PLF_NO_PATH,
	CCST_PLF_OPEN,
	CCST_PLF_READ,
	CCST_PLF_MAGIC,
	CCST_PLF_VERSION
};

static cc_bool CCST_CopyToRaw(char* dst, int capacity, const cc_string* src) {
	int len;
	if (!dst || capacity <= 0) return false;
	dst[0] = '\0';
	if (!src || !src->buffer) return false;

	len = src->length;
	if (len < 0) len = 0;
	if (len > capacity - 1) len = capacity - 1;
	if (len > 0) memcpy(dst, src->buffer, (size_t)len);
	dst[len] = '\0';
	return true;
}

#define CCST_PERSIST_FLAG_POS_DENIED_WARNED 0x0001
#define CCST_PERSIST_FLAG_MODE_SURVIVAL     0x0002

static void CCST_Persist_OptionsSetDefaults(void) {
	ccst_opt_default_survival = false; /* creative */
	ccst_opt_autosave         = true;
	ccst_opt_firsttime_seen   = false;
}

static cc_bool CCST_Persist_ParseBool(const cc_string* s, cc_bool defVal) {
	if (!s || !s->length) return defVal;
	if (String_CaselessEqualsConst(s, "1") || String_CaselessEqualsConst(s, "true") || String_CaselessEqualsConst(s, "yes") || String_CaselessEqualsConst(s, "on"))
		return true;
	if (String_CaselessEqualsConst(s, "0") || String_CaselessEqualsConst(s, "false") || String_CaselessEqualsConst(s, "no") || String_CaselessEqualsConst(s, "off"))
		return false;
	return defVal;
}

static void CCST_Persist_SaveOptions(void) {
	struct Stream stream;
	cc_result res;
	cc_string path = String_FromConst("ccst/options.cfg");
	cc_string line;
	char lineBuf[128];

	if (!CCST_Persist_EnsureDir("ccst")) return;
	res = Stream_CreateFile(&stream, &path);
	if (res) return;

	String_InitArray(line, lineBuf);
	String_AppendConst(&line, "default_gamemode=");
	String_AppendConst(&line, ccst_opt_default_survival ? "survival" : "creative");
	(void)Stream_WriteLine(&stream, &line);

	line.length = 0;
	String_AppendConst(&line, "autosave=");
	String_AppendConst(&line, ccst_opt_autosave ? "true" : "false");
	(void)Stream_WriteLine(&stream, &line);

	line.length = 0;
	String_AppendConst(&line, "seenintro=");
	String_AppendConst(&line, ccst_opt_firsttime_seen ? "true" : "false");
	(void)Stream_WriteLine(&stream, &line);

	(void)stream.Close(&stream);
}

static void CCST_Persist_LoadOptions(void) {
	struct Stream stream, buffered;
	cc_result res;
	cc_uint8 buf[2048];
	cc_string path = String_FromConst("ccst/options.cfg");
	cc_string entry, key, value;
	char entryBuf[256];

	CCST_Persist_OptionsSetDefaults();
	if (!CCST_Persist_EnsureDir("ccst")) return;

	res = Stream_OpenFile(&stream, &path);
	if (res) {
		CCST_Persist_SaveOptions();
		return;
	}

	Stream_ReadonlyBuffered(&buffered, &stream, buf, sizeof(buf));
	for (;;) {
		String_InitArray(entry, entryBuf);
		res = Stream_ReadLine(&buffered, &entry);
		if (res == ERR_END_OF_STREAM) break;
		if (res) break;

		String_UNSAFE_TrimStart(&entry);
		String_UNSAFE_TrimEnd(&entry);
		if (!entry.length || entry.buffer[0] == '#') continue;
		if (String_IndexOf(&entry, '=') < 0) continue;

		String_UNSAFE_Separate(&entry, '=', &key, &value);
		String_UNSAFE_TrimStart(&key); String_UNSAFE_TrimEnd(&key);
		String_UNSAFE_TrimStart(&value); String_UNSAFE_TrimEnd(&value);

		if (String_CaselessEqualsConst(&key, "default_gamemode")) {
			if (String_CaselessEqualsConst(&value, "survival")) ccst_opt_default_survival = true;
			else if (String_CaselessEqualsConst(&value, "creative")) ccst_opt_default_survival = false;
		} else if (String_CaselessEqualsConst(&key, "autosave")) {
			ccst_opt_autosave = CCST_Persist_ParseBool(&value, ccst_opt_autosave);
		} else if (String_CaselessEqualsConst(&key, "seenintro")) {
			ccst_opt_firsttime_seen = CCST_Persist_ParseBool(&value, ccst_opt_firsttime_seen);
		}
	}
	(void)stream.Close(&stream);
}

static cc_bool CCST_Persist_EnsureDir(const char* path) {
	cc_string s = String_FromReadonly(path);
	cc_filepath raw;
	cc_result res;
	Platform_EncodePath(&raw, &s);
	res = Directory_Create2(&raw);
	return res == 0 || res == CCST_ALREADY_EXISTS;
}

static void CCST_Persist_SanitizeFolderName(cc_string* dst, char* dstBuf, int dstCap, const cc_string* src) {
	int i, maxLen;
	char c;

	dst->buffer   = dstBuf;
	dst->length   = 0;
	dst->capacity = dstCap;
	maxLen = dstCap - 1;
	if (maxLen > 80) maxLen = 80;

	for (i = 0; i < src->length; i++) {
		c = src->buffer[i];
		if (dst->length >= maxLen) break;
		if (c == '&' && i + 1 < src->length) { i++; continue; }

		if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') {
			String_Append(dst, '_');
		} else if (c != '\r' && c != '\n' && c >= 32 && c != 127) {
			String_Append(dst, c);
		}
	}
	String_UNSAFE_TrimStart(dst);
	String_UNSAFE_TrimEnd(dst);
}

static cc_bool CCST_Persist_ServerDir(char* out, int outCap) {
	cc_string addrSan;
	char addrBuf[128];
	cc_string path;
	char pathBuf[FILENAME_SIZE];

	if (outCap < 32) return false;
	if (!CCST_Persist_EnsureDir("ccst")) return false;
	if (!CCST_Persist_EnsureDir("ccst/servers")) return false;

	if (Server.IsSinglePlayer) {
		String_InitArray(addrSan, addrBuf);
		String_AppendConst(&addrSan, "singleplayer");
	} else {
		CCST_Persist_SanitizeFolderName(&addrSan, addrBuf, sizeof(addrBuf), &Server.Address);
		if (!addrSan.length) String_AppendConst(&addrSan, "server");
	}

	String_InitArray(path, pathBuf);
	String_AppendConst(&path, "ccst/servers/");
	String_AppendString(&path, &addrSan);
	if (!Server.IsSinglePlayer) {
		String_AppendConst(&path, "_");
		String_AppendInt(&path, Server.Port);
	}

	if (!CCST_CopyToRaw(out, outCap, &path)) return false;
	return CCST_Persist_EnsureDir(out);
}

static cc_uint64 CCST_XorShift64(cc_uint64 s) {
	s ^= s >> 12;
	s ^= s << 25;
	s ^= s >> 27;
	return s * 2685821657736338717ULL;
}

static void CCST_BlockEmbedding(BlockID b, float out[CCST_EMB_D]) {
	cc_uint64 state = ((cc_uint64)b + 1ULL) * 0x9e3779b97f4a7c15ULL;
	double norm2 = 0.0;
	int i;

	for (i = 0; i < CCST_EMB_D; i++) {
		double u;
		state = CCST_XorShift64(state);
		u = (double)(state & 0xFFFFFFFFULL) / (double)0xFFFFFFFFULL;
		u = u * 2.0 - 1.0;
		out[i] = (float)u;
		norm2 += u * u;
	}

	if (norm2 <= 0.0) norm2 = 1.0;
	{
		double inv = 1.0 / sqrt(norm2);
		for (i = 0; i < CCST_EMB_D; i++) out[i] = (float)((double)out[i] * inv);
	}
}

static int CCST_BlockClassOf(BlockID b) {
	if (b == BLOCK_AIR) return CCST_CLASS_AIR;
	if (b >= BLOCK_COUNT) return CCST_CLASS_OTHER;
	if (Blocks.Draw[b] == DRAW_GAS) return CCST_CLASS_GAS;
	if (Blocks.Draw[b] == DRAW_SPRITE) return CCST_CLASS_SPRITE;
	if (Blocks.Collide[b] == COLLIDE_LIQUID) return CCST_CLASS_LIQUID;
	if (Blocks.Collide[b] == COLLIDE_SOLID) return CCST_CLASS_SOLID;
	return CCST_CLASS_OTHER;
}

static void CCST_SelectFrequencies(int dim, int count, const int* wavelengths, int* outK) {
	int i;
	if (dim <= 1) {
		for (i = 0; i < count; i++) outK[i] = 0;
		return;
	}

	for (i = 0; i < count; i++) {
		int wl = wavelengths[i];
		int k  = (dim + wl / 2) / wl; /* rounded dim / wavelength */
		if (k < 1) k = 1;
		if (k > dim - 1) k = dim - 1;
		outK[i] = k;
	}
}

static void CCST_NormalizeVector(float* v, int len) {
	double n2 = 0.0;
	int i;
	for (i = 0; i < len; i++) n2 += (double)v[i] * (double)v[i];
	if (n2 <= 0.0) return;
	{
		float inv = (float)(1.0 / sqrt(n2));
		for (i = 0; i < len; i++) v[i] *= inv;
	}
}

/* Cheap world-change detector used to defer expensive descriptor computation until map settles. */
static cc_uint64 CCST_Persist_QuickWorldFingerprint(void) {
	cc_uint64 h = 1469598103934665603ULL;
	int w = World.Width, hh = World.Height, l = World.Length;
	int i;
	if (!World.Blocks || w <= 0 || hh <= 0 || l <= 0) return 0;

	for (i = 0; i < CCST_RESTORE_SAMPLE_COUNT; i++) {
		cc_uint64 s = (cc_uint64)(i + 1) * 0x9e3779b97f4a7c15ULL;
		int x = (int)((s >>  0) % (cc_uint64)w);
		int y = (int)((s >> 21) % (cc_uint64)hh);
		int z = (int)((s >> 42) % (cc_uint64)l);
		BlockID b = World_GetBlock(x, y, z);
		h ^= (cc_uint64)(b + 1);
		h *= 1099511628211ULL;
	}
	return h;
}

static cc_bool CCST_Persist_ComputeDescriptor(struct CCST_WorldDescriptor* out) {
	int w, h, l;
	int cx, cy, cz;
	int cells;
	float* cellVec;
	cc_uint32* cellCount;
	float embCache[BLOCK_COUNT][CCST_EMB_D];
	cc_bool embReady[BLOCK_COUNT];
	double* cosXLow[CCST_LOW_KX];
	double* cosYLow[CCST_LOW_KY];
	double* cosZLow[CCST_LOW_KZ];
	double* cosXMid[CCST_MID_KX];
	double* cosYMid[CCST_MID_KY];
	double* cosZMid[CCST_MID_KZ];
	double* cosXHigh[CCST_HIGH_KX];
	double* cosYHigh[CCST_HIGH_KY];
	double* cosZHigh[CCST_HIGH_KZ];
	int midKx[CCST_MID_KX], midKy[CCST_MID_KY], midKz[CCST_MID_KZ];
	int highKx[CCST_HIGH_KX], highKy[CCST_HIGH_KY], highKz[CCST_HIGH_KZ];
	static const int wlMid[4]  = { 8, 16, 32, 64 };
	static const int wlHigh[5] = { 2, 4, 8, 16, 32 };
	int i, x, y, z;
	int totalVoxels;
	const double TWO_PI = 2.0 * acos(-1.0);

	if (!out || !World.Blocks) return false;
	w = World.Width; h = World.Height; l = World.Length;
	if (w <= 0 || h <= 0 || l <= 0) return false;

	cx = w < CCST_COARSE_MAX_X ? w : CCST_COARSE_MAX_X;
	cy = h < CCST_COARSE_MAX_Y ? h : CCST_COARSE_MAX_Y;
	cz = l < CCST_COARSE_MAX_Z ? l : CCST_COARSE_MAX_Z;
	if (cx <= 0) cx = 1;
	if (cy <= 0) cy = 1;
	if (cz <= 0) cz = 1;

	cells = cx * cy * cz;
	cellVec = (float*)Mem_TryAllocCleared(cells * CCST_EMB_D, sizeof(float));
	if (!cellVec) return false;
	cellCount = (cc_uint32*)Mem_TryAllocCleared(cells, sizeof(cc_uint32));
	if (!cellCount) { Mem_Free(cellVec); return false; }

	memset(out, 0, sizeof(*out));
	out->coarseX = (cc_uint16)cx;
	out->coarseY = (cc_uint16)cy;
	out->coarseZ = (cc_uint16)cz;

	memset(embReady, 0, sizeof(embReady));
	totalVoxels = 0;

	/* Strided sampling: for very large worlds, iterating every block is O(N^3) and too slow.
	 * We only need enough samples per coarse cell to get a representative average.
	 * Max samples per coarse dimension = 8 -> max 512 samples per cell. */
	#define CCST_MAX_SAMPLES_PER_CELL_DIM 8
	int stepX = w / (cx * CCST_MAX_SAMPLES_PER_CELL_DIM);
	int stepY = h / (cy * CCST_MAX_SAMPLES_PER_CELL_DIM);
	int stepZ = l / (cz * CCST_MAX_SAMPLES_PER_CELL_DIM);
	if (stepX < 1) stepX = 1;
	if (stepY < 1) stepY = 1;
	if (stepZ < 1) stepZ = 1;

	for (y = 0; y < h; y += stepY) {
		int yy = (int)(((cc_uint64)y * cy) / h);
		if (yy >= cy) yy = cy - 1;
		for (z = 0; z < l; z += stepZ) {
			int zz = (int)(((cc_uint64)z * cz) / l);
			if (zz >= cz) zz = cz - 1;
			for (x = 0; x < w; x += stepX) {
				BlockID b = World_GetBlock(x, y, z);
				int xx = (int)(((cc_uint64)x * cx) / w);
				if (xx >= cx) xx = cx - 1;
				int cidx = ((yy * cz) + zz) * cx + xx;
				float* dst = &cellVec[cidx * CCST_EMB_D];
				int cls;

				if (b >= BLOCK_COUNT) b %= BLOCK_COUNT;
				if (!embReady[b]) {
					CCST_BlockEmbedding(b, embCache[b]);
					embReady[b] = true;
				}

				for (i = 0; i < CCST_EMB_D; i++) dst[i] += embCache[b][i];
				cellCount[cidx]++;

				cls = CCST_BlockClassOf(b);
				if (cls < 0 || cls >= CCST_HIST_BINS) cls = CCST_CLASS_OTHER;
				out->hist[cls] += 1.0f;
				totalVoxels++;
			}
		}
	}

	for (i = 0; i < cells; i++) {
		cc_uint32 n = cellCount[i];
		float inv = n ? (1.0f / (float)n) : 0.0f;
		int j;
		for (j = 0; j < CCST_EMB_D; j++) cellVec[i * CCST_EMB_D + j] *= inv;
	}

	if (totalVoxels > 0) {
		float inv = 1.0f / (float)totalVoxels;
		for (i = 0; i < CCST_HIST_BINS; i++) out->hist[i] *= inv;
	}

	for (i = 0; i < CCST_LOW_KX; i++) {
		int k = i;
		int xx;
		cosXLow[i] = (double*)Mem_TryAlloc(cx, sizeof(double));
		if (!cosXLow[i]) goto fail;
		for (xx = 0; xx < cx; xx++) cosXLow[i][xx] = cos(TWO_PI * (double)k * (double)xx / (double)cx);
	}
	for (i = 0; i < CCST_LOW_KY; i++) {
		int k = i;
		int yy;
		cosYLow[i] = (double*)Mem_TryAlloc(cy, sizeof(double));
		if (!cosYLow[i]) goto fail;
		for (yy = 0; yy < cy; yy++) cosYLow[i][yy] = cos(TWO_PI * (double)k * (double)yy / (double)cy);
	}
	for (i = 0; i < CCST_LOW_KZ; i++) {
		int k = i;
		int zz;
		cosZLow[i] = (double*)Mem_TryAlloc(cz, sizeof(double));
		if (!cosZLow[i]) goto fail;
		for (zz = 0; zz < cz; zz++) cosZLow[i][zz] = cos(TWO_PI * (double)k * (double)zz / (double)cz);
	}

	CCST_SelectFrequencies(cx, CCST_MID_KX, wlMid, midKx);
	CCST_SelectFrequencies(cy, CCST_MID_KY, wlMid, midKy);
	CCST_SelectFrequencies(cz, CCST_MID_KZ, wlMid, midKz);

	for (i = 0; i < CCST_MID_KX; i++) {
		int xx;
		cosXMid[i] = (double*)Mem_TryAlloc(cx, sizeof(double));
		if (!cosXMid[i]) goto fail;
		for (xx = 0; xx < cx; xx++) cosXMid[i][xx] = cos(TWO_PI * (double)midKx[i] * (double)xx / (double)cx);
	}
	for (i = 0; i < CCST_MID_KY; i++) {
		int yy;
		cosYMid[i] = (double*)Mem_TryAlloc(cy, sizeof(double));
		if (!cosYMid[i]) goto fail;
		for (yy = 0; yy < cy; yy++) cosYMid[i][yy] = cos(TWO_PI * (double)midKy[i] * (double)yy / (double)cy);
	}
	for (i = 0; i < CCST_MID_KZ; i++) {
		int zz;
		cosZMid[i] = (double*)Mem_TryAlloc(cz, sizeof(double));
		if (!cosZMid[i]) goto fail;
		for (zz = 0; zz < cz; zz++) cosZMid[i][zz] = cos(TWO_PI * (double)midKz[i] * (double)zz / (double)cz);
	}

	CCST_SelectFrequencies(cx, CCST_HIGH_KX, wlHigh, highKx);
	CCST_SelectFrequencies(cy, CCST_HIGH_KY, wlHigh, highKy);
	CCST_SelectFrequencies(cz, CCST_HIGH_KZ, wlHigh, highKz);

	for (i = 0; i < CCST_HIGH_KX; i++) {
		int xx;
		cosXHigh[i] = (double*)Mem_TryAlloc(cx, sizeof(double));
		if (!cosXHigh[i]) goto fail;
		for (xx = 0; xx < cx; xx++) cosXHigh[i][xx] = cos(TWO_PI * (double)highKx[i] * (double)xx / (double)cx);
	}
	for (i = 0; i < CCST_HIGH_KY; i++) {
		int yy;
		cosYHigh[i] = (double*)Mem_TryAlloc(cy, sizeof(double));
		if (!cosYHigh[i]) goto fail;
		for (yy = 0; yy < cy; yy++) cosYHigh[i][yy] = cos(TWO_PI * (double)highKy[i] * (double)yy / (double)cy);
	}
	for (i = 0; i < CCST_HIGH_KZ; i++) {
		int zz;
		cosZHigh[i] = (double*)Mem_TryAlloc(cz, sizeof(double));
		if (!cosZHigh[i]) goto fail;
		for (zz = 0; zz < cz; zz++) cosZHigh[i][zz] = cos(TWO_PI * (double)highKz[i] * (double)zz / (double)cz);
	}

	{
		int kx, ky, kz, basis;
		float invCells = cells > 0 ? (1.0f / (float)cells) : 0.0f;
		memset(out->low, 0, sizeof(out->low));
		memset(out->mid, 0, sizeof(out->mid));
		memset(out->high, 0, sizeof(out->high));

		for (ky = 0; ky < CCST_LOW_KY; ky++) for (kx = 0; kx < CCST_LOW_KX; kx++) for (kz = 0; kz < CCST_LOW_KZ; kz++) {
			basis = (ky * CCST_LOW_KX + kx) * CCST_LOW_KZ + kz;
			for (y = 0; y < cy; y++) for (z = 0; z < cz; z++) for (x = 0; x < cx; x++) {
				double wgt = cosXLow[kx][x] * cosYLow[ky][y] * cosZLow[kz][z];
				int cidx = ((y * cz) + z) * cx + x;
				float* src = &cellVec[cidx * CCST_EMB_D];
				for (i = 0; i < CCST_EMB_D; i++) out->low[basis * CCST_EMB_D + i] += (float)((double)src[i] * wgt);
			}
			for (i = 0; i < CCST_EMB_D; i++) out->low[basis * CCST_EMB_D + i] *= invCells;
		}

		for (ky = 0; ky < CCST_MID_KY; ky++) for (kx = 0; kx < CCST_MID_KX; kx++) for (kz = 0; kz < CCST_MID_KZ; kz++) {
			basis = (ky * CCST_MID_KX + kx) * CCST_MID_KZ + kz;
			for (y = 0; y < cy; y++) for (z = 0; z < cz; z++) for (x = 0; x < cx; x++) {
				double wgt = cosXMid[kx][x] * cosYMid[ky][y] * cosZMid[kz][z];
				int cidx = ((y * cz) + z) * cx + x;
				float* src = &cellVec[cidx * CCST_EMB_D];
				for (i = 0; i < CCST_EMB_D; i++) out->mid[basis * CCST_EMB_D + i] += (float)((double)src[i] * wgt);
			}
			for (i = 0; i < CCST_EMB_D; i++) out->mid[basis * CCST_EMB_D + i] *= invCells;
		}

		for (ky = 0; ky < CCST_HIGH_KY; ky++) for (kx = 0; kx < CCST_HIGH_KX; kx++) for (kz = 0; kz < CCST_HIGH_KZ; kz++) {
			basis = (ky * CCST_HIGH_KX + kx) * CCST_HIGH_KZ + kz;
			for (y = 0; y < cy; y++) for (z = 0; z < cz; z++) for (x = 0; x < cx; x++) {
				double wgt = cosXHigh[kx][x] * cosYHigh[ky][y] * cosZHigh[kz][z];
				int cidx = ((y * cz) + z) * cx + x;
				float* src = &cellVec[cidx * CCST_EMB_D];
				for (i = 0; i < CCST_EMB_D; i++) out->high[basis * CCST_EMB_D + i] += (float)((double)src[i] * wgt);
			}
			for (i = 0; i < CCST_EMB_D; i++) out->high[basis * CCST_EMB_D + i] *= invCells;
		}
	}

	CCST_NormalizeVector(out->low, CCST_LOW_LEN);
	CCST_NormalizeVector(out->mid, CCST_MID_LEN);
	CCST_NormalizeVector(out->high, CCST_HIGH_LEN);
	CCST_NormalizeVector(out->hist, CCST_HIST_BINS);

	for (i = 0; i < CCST_LOW_KX; i++) Mem_Free(cosXLow[i]);
	for (i = 0; i < CCST_LOW_KY; i++) Mem_Free(cosYLow[i]);
	for (i = 0; i < CCST_LOW_KZ; i++) Mem_Free(cosZLow[i]);
	for (i = 0; i < CCST_MID_KX; i++) Mem_Free(cosXMid[i]);
	for (i = 0; i < CCST_MID_KY; i++) Mem_Free(cosYMid[i]);
	for (i = 0; i < CCST_MID_KZ; i++) Mem_Free(cosZMid[i]);
	for (i = 0; i < CCST_HIGH_KX; i++) Mem_Free(cosXHigh[i]);
	for (i = 0; i < CCST_HIGH_KY; i++) Mem_Free(cosYHigh[i]);
	for (i = 0; i < CCST_HIGH_KZ; i++) Mem_Free(cosZHigh[i]);
	Mem_Free(cellVec);
	Mem_Free(cellCount);
	return true;

fail:
	for (i = 0; i < CCST_LOW_KX; i++) if (cosXLow[i]) Mem_Free(cosXLow[i]);
	for (i = 0; i < CCST_LOW_KY; i++) if (cosYLow[i]) Mem_Free(cosYLow[i]);
	for (i = 0; i < CCST_LOW_KZ; i++) if (cosZLow[i]) Mem_Free(cosZLow[i]);
	for (i = 0; i < CCST_MID_KX; i++) if (cosXMid[i]) Mem_Free(cosXMid[i]);
	for (i = 0; i < CCST_MID_KY; i++) if (cosYMid[i]) Mem_Free(cosYMid[i]);
	for (i = 0; i < CCST_MID_KZ; i++) if (cosZMid[i]) Mem_Free(cosZMid[i]);
	for (i = 0; i < CCST_HIGH_KX; i++) if (cosXHigh[i]) Mem_Free(cosXHigh[i]);
	for (i = 0; i < CCST_HIGH_KY; i++) if (cosYHigh[i]) Mem_Free(cosYHigh[i]);
	for (i = 0; i < CCST_HIGH_KZ; i++) if (cosZHigh[i]) Mem_Free(cosZHigh[i]);
	Mem_Free(cellVec);
	Mem_Free(cellCount);
	return false;
}

static float CCST_L2Distance(const float* a, const float* b, int len) {
	double acc = 0.0;
	int i;
	for (i = 0; i < len; i++) {
		double d = (double)a[i] - (double)b[i];
		acc += d * d;
	}
	return (float)sqrt(acc);
}

static float CCST_DescriptorScore(const struct CCST_WorldDescriptor* a, const struct CCST_WorldDescriptor* b) {
	float dLow, dMid, dHigh, dHist;
	if (!a || !b) return FLT_MAX;
	dLow  = CCST_L2Distance(a->low,  b->low,  CCST_LOW_LEN);
	dMid  = CCST_L2Distance(a->mid,  b->mid,  CCST_MID_LEN);
	dHigh = CCST_L2Distance(a->high, b->high, CCST_HIGH_LEN);
	dHist = CCST_L2Distance(a->hist, b->hist, CCST_HIST_BINS);
	return CCST_MATCH_W_LOW * dLow + CCST_MATCH_W_MID * dMid + CCST_MATCH_W_HIGH * dHigh + CCST_MATCH_W_HIST * dHist;
}

static void CCST_Persist_FileNameFromDescriptor(char* out, int outCap, const struct CCST_WorldDescriptor* d) {
	unsigned long long h = 1469598103934665603ULL;
	static const char* hex = "0123456789abcdef";
	char buf[64];
	cc_string s;
	int i;

	/* Mix a few components from each cascade for the filename hash. */
	for (i = 0; i < 4 && i < CCST_LOW_LEN; i++) {
		union { float f; cc_uint32 u; } conv;
		conv.f = d->low[i];
		h ^= (cc_uint64)conv.u; h *= 1099511628211ULL;
	}
	for (i = 0; i < 4 && i < CCST_MID_LEN; i++) {
		union { float f; cc_uint32 u; } conv;
		conv.f = d->mid[i];
		h ^= (cc_uint64)conv.u; h *= 1099511628211ULL;
	}
	for (i = 0; i < 4 && i < CCST_HIGH_LEN; i++) {
		union { float f; cc_uint32 u; } conv;
		conv.f = d->high[i];
		h ^= (cc_uint64)conv.u; h *= 1099511628211ULL;
	}

	String_InitArray(s, buf);
	String_AppendConst(&s, "world_");
	for (i = 60; i >= 0; i -= 4) {
		char c = hex[(h >> i) & 0xFULL];
		String_Append(&s, c);
	}
	String_AppendConst(&s, ".bin");
	CCST_CopyToRaw(out, outCap, &s);
}

static cc_bool CCST_Persist_BuildPath(char* out, int outCap, const struct CCST_PersistFile* f) {
	char dir[FILENAME_SIZE], fname[64], pathBuf[FILENAME_SIZE];
	cc_string dirStr, fileStr, path;
	if (!CCST_Persist_ServerDir(dir, sizeof(dir))) return false;
	CCST_Persist_FileNameFromDescriptor(fname, sizeof(fname), &f->desc);
	dirStr  = String_FromReadonly(dir);
	fileStr = String_FromReadonly(fname);
	String_InitArray(path, pathBuf);
	String_AppendString(&path, &dirStr);
	String_AppendConst(&path, "/");
	String_AppendString(&path, &fileStr);
	return CCST_CopyToRaw(out, outCap, &path);
}

static void CCST_Persist_FillMeta(struct CCST_PersistFile* f) {
	struct LocalPlayer* p = Entities.CurPlayer;
	int i;
	memset(f, 0, sizeof(*f));
	f->magic = CCST_PERSIST_MAGIC;
	f->version = CCST_PERSIST_VER;
	f->flags = 0;
	if (ccst_persist_posDeniedWarned) f->flags |= CCST_PERSIST_FLAG_POS_DENIED_WARNED;
	if (CCST_Health_GetModeSurvival()) f->flags |= CCST_PERSIST_FLAG_MODE_SURVIVAL;
	f->worldW = (cc_uint16)World.Width;
	f->worldH = (cc_uint16)World.Height;
	f->worldL = (cc_uint16)World.Length;
	f->isSingleplayer = Server.IsSinglePlayer ? 1 : 0;
	f->serverPort = (cc_uint32)Server.Port;

	if (p) {
		f->px = p->Base.Position.x;
		f->py = p->Base.Position.y;
		f->pz = p->Base.Position.z;
		f->yaw = p->Base.Yaw;
		f->pitch = p->Base.Pitch;
	}

	f->health = (cc_int32)CCST_Health_Get();
	f->score  = (cc_int32)CCST_Score_Get();
	CCST_Inv_GetState(f->blocks, f->counts, &f->transmuteBlock, &f->transmuteCount);
	for (i = 0; i < CCST_INV_SLOTS; i++) if (f->counts[i] > 64) f->counts[i] = 64;
}

static void CCST_Persist_TryRestoreGamemode(const struct CCST_PersistFile* f) {
	cc_bool savedMode;
	if (!f) return;
	savedMode = (f->flags & CCST_PERSIST_FLAG_MODE_SURVIVAL) != 0;

	/* Respect world policy: if mode is forced and conflicts with saved mode, keep forced mode. */
	if (CCST_Policy_IsGamemodeForced()) {
		if (CCST_Health_GetModeSurvival() != savedMode) {
			CCST_Chat("&eSaved gamemode differs from this world's policy, so it has been changed.");
		}
		return;
	}
	CCST_Health_SetModeSurvival(savedMode);
}

static cc_bool CCST_Persist_LoadFileExact(const char* path, struct CCST_PersistFile* out, int* failReason) {
	cc_string p;
	struct Stream stream;
	cc_result res;
	if (failReason) *failReason = CCST_PLF_NONE;
	if (!path || !path[0]) {
		if (failReason) *failReason = CCST_PLF_NO_PATH;
		return false;
	}
	p = String_FromReadonly(path);
	res = Stream_OpenFile(&stream, &p);
	if (res) {
		if (failReason) *failReason = CCST_PLF_OPEN;
		return false;
	}
	res = Stream_Read(&stream, (cc_uint8*)out, (cc_uint32)sizeof(*out));
	(void)stream.Close(&stream);
	if (res) {
		if (failReason) *failReason = CCST_PLF_READ;
		return false;
	}
	if (out->magic != CCST_PERSIST_MAGIC) {
		if (failReason) *failReason = CCST_PLF_MAGIC;
		return false;
	}
	if (out->version != CCST_PERSIST_VER) {
		if (failReason) *failReason = CCST_PLF_VERSION;
		return false;
	}
	return true;
}

static struct CCST_WorldDescriptor ccst_persist_cachedDesc;
static cc_bool ccst_persist_needsDescriptor = true;

void CCST_Persist_MarkDirty(void) {
	ccst_persist_needsDescriptor = true;
}

static cc_bool CCST_Persist_SaveNow(void) {
	struct CCST_PersistFile f;
	char path[FILENAME_SIZE];
	struct Stream s;
	cc_string p;
	cc_result res;

	if (!CCST_Policy_PluginEnabled()) return false;
	if (!World.Loaded || !World.Blocks) return false;
	if (Server.Disconnected) return false;

	if (ccst_persist_needsDescriptor) {
		if (CCST_Persist_ComputeDescriptor(&ccst_persist_cachedDesc)) {
			ccst_persist_needsDescriptor = false;
		}
	}

	CCST_Persist_FillMeta(&f);
	memcpy(&f.desc, &ccst_persist_cachedDesc, sizeof(f.desc));

	if (ccst_persist_hasActivePath) {
		cc_string active = String_FromReadonly(ccst_persist_activePath);
		if (!CCST_CopyToRaw(path, sizeof(path), &active)) return false;
	} else {
		cc_string built;
		if (!CCST_Persist_BuildPath(path, sizeof(path), &f)) return false;
		built = String_FromReadonly(path);
		CCST_CopyToRaw(ccst_persist_activePath, sizeof(ccst_persist_activePath), &built);
		ccst_persist_hasActivePath = true;
	}

	p = String_FromReadonly(path);
	res = Stream_CreateFile(&s, &p);
	if (res) return false;
	res = Stream_Write(&s, (const cc_uint8*)&f, (cc_uint32)sizeof(f));
	{
		cc_result closeRes = s.Close(&s);
		if (res) return false;
		if (closeRes) return false;
	}
	return true;
}

struct CCST_PersistScanCtx {
	const struct CCST_WorldDescriptor* cur;
	float best;
	float second;
	int filesSeen;
	int filesLoaded;
	int filesDimsRejected;
	int loadFailNoPath;
	int loadFailOpen;
	int loadFailRead;
	int loadFailMagic;
	int loadFailVersion;
	struct CCST_PersistFile bestFile;
	cc_bool hasBestPath;
	char bestPath[FILENAME_SIZE];
	cc_string dirStr;
	char dirBuf[FILENAME_SIZE];
};

static void CCST_Persist_ScanCallback(const cc_string* filename, void* obj, int isDirectory) {
	struct CCST_PersistScanCtx* ctx = (struct CCST_PersistScanCtx*)obj;
	struct CCST_PersistFile tmp;
	char pathBuf[FILENAME_SIZE];
	char nameBuf[FILENAME_SIZE];
	cc_string path;
	float score;
	int n;
	int failReason;
	int hasSlash = 0;
	int i;

	if (isDirectory) return;
	n = filename->length;
	if (n < 4) return;
	/* Accept any .bin candidate; file validity is checked by magic/version on load. */
	if (filename->buffer[n - 4] != '.' || filename->buffer[n - 3] != 'b' ||
		filename->buffer[n - 2] != 'i' || filename->buffer[n - 1] != 'n') return;
	ctx->filesSeen++;
	if (n >= (int)sizeof(nameBuf)) return;
	memcpy(nameBuf, filename->buffer, (size_t)n);
	nameBuf[n] = '\0';
	for (i = 0; i < n; i++) {
		if (nameBuf[i] == '/' || nameBuf[i] == '\\') {
			hasSlash = 1;
			break;
		}
	}

	String_InitArray(path, pathBuf);
	/* Directory_Enum may return either a basename or a relative path including directories. */
	if (hasSlash) {
		String_AppendConst(&path, nameBuf);
	} else {
		String_AppendString(&path, &ctx->dirStr);
		String_AppendConst(&path, "/");
		String_AppendConst(&path, nameBuf);
	}

	if (path.length <= 0 || path.length >= (int)sizeof(pathBuf)) {
		ctx->loadFailNoPath++;
		return;
	}
	/* String_Append does not guarantee trailing NUL; Stream_OpenFile path must be C-string backed. */
	pathBuf[path.length] = '\0';
	if (!CCST_Persist_LoadFileExact(pathBuf, &tmp, &failReason)) {
		if (failReason == CCST_PLF_NO_PATH) ctx->loadFailNoPath++;
		else if (failReason == CCST_PLF_OPEN) ctx->loadFailOpen++;
		else if (failReason == CCST_PLF_READ) ctx->loadFailRead++;
		else if (failReason == CCST_PLF_MAGIC) ctx->loadFailMagic++;
		else if (failReason == CCST_PLF_VERSION) ctx->loadFailVersion++;
#if CCST_PERSIST_DEBUG
		{
			Chat_Add2("&ePersist dbg: load fail reason=%i file=%s", &failReason, filename);
		}
#endif
		return;
	}
	ctx->filesLoaded++;
	if (tmp.worldW != (cc_uint16)World.Width || tmp.worldH != (cc_uint16)World.Height || tmp.worldL != (cc_uint16)World.Length) {
		ctx->filesDimsRejected++;
		return;
	}

	score = CCST_DescriptorScore(ctx->cur, &tmp.desc);
	if (score < ctx->best) {
		ctx->second = ctx->best;
		ctx->best   = score;
		memcpy(&ctx->bestFile, &tmp, sizeof(tmp));
		CCST_CopyToRaw(ctx->bestPath, sizeof(ctx->bestPath), &path);
		ctx->hasBestPath = true;
	} else if (score < ctx->second) {
		ctx->second = score;
	}
}

static cc_bool CCST_Persist_FindBestCandidate(struct CCST_PersistFile* out, char* outPath, int outPathCap, const struct CCST_WorldDescriptor* cur) {
	char dir[FILENAME_SIZE];
	struct CCST_PersistScanCtx ctx;
	float margin;

	if (!CCST_Persist_ServerDir(dir, sizeof(dir))) return false;

	String_InitArray(ctx.dirStr, ctx.dirBuf);
	String_AppendConst(&ctx.dirStr, dir);
	ctx.cur = cur;
	ctx.best = FLT_MAX;
	ctx.second = FLT_MAX;
	ctx.filesSeen = 0;
	ctx.filesLoaded = 0;
	ctx.filesDimsRejected = 0;
	ctx.loadFailNoPath = 0;
	ctx.loadFailOpen = 0;
	ctx.loadFailRead = 0;
	ctx.loadFailMagic = 0;
	ctx.loadFailVersion = 0;
	ctx.hasBestPath = false;
	ctx.bestPath[0] = '\0';
	memset(&ctx.bestFile, 0, sizeof(ctx.bestFile));

	Directory_Enum(&ctx.dirStr, &ctx, CCST_Persist_ScanCallback);
	ccst_persist_lastBestScore = ctx.best;
	ccst_persist_lastSecondScore = ctx.second;
	if (ctx.best == FLT_MAX) {
#if CCST_PERSIST_DEBUG
		Chat_Add3("&ePersist dbg: loadfail open=%i read=%i magic=%i", &ctx.loadFailOpen, &ctx.loadFailRead, &ctx.loadFailMagic);
		Chat_Add2("&ePersist dbg: loadfail version=%i nopath=%i", &ctx.loadFailVersion, &ctx.loadFailNoPath);
		Chat_Add3("&ePersist dbg: scan no-match seen=%i loaded=%i dimReject=%i", &ctx.filesSeen, &ctx.filesLoaded, &ctx.filesDimsRejected);
#endif
		return false;
	}
	/* Always expose most-likely candidate to caller (even if not confident enough for auto-restore). */
	memcpy(out, &ctx.bestFile, sizeof(*out));
	if (outPath && outPathCap > 0 && ctx.hasBestPath) {
		cc_string s = String_FromReadonly(ctx.bestPath);
		CCST_CopyToRaw(outPath, outPathCap, &s);
	}

	margin = (ctx.second == FLT_MAX) ? 1.0f : (ctx.second - ctx.best);
#if CCST_PERSIST_DEBUG
	Chat_Add3("&ePersist dbg: loadfail open=%i read=%i magic=%i", &ctx.loadFailOpen, &ctx.loadFailRead, &ctx.loadFailMagic);
	Chat_Add2("&ePersist dbg: loadfail version=%i nopath=%i", &ctx.loadFailVersion, &ctx.loadFailNoPath);
	Chat_Add3("&ePersist dbg: scan seen=%i loaded=%i dimReject=%i", &ctx.filesSeen, &ctx.filesLoaded, &ctx.filesDimsRejected);
	Chat_Add3("&ePersist dbg: best=%f second=%f margin=%f", &ctx.best, &ctx.second, &margin);
#endif
	if (ctx.best <= CCST_MATCH_THRESHOLD &&
		(ctx.best <= CCST_MATCH_STRONG_ACCEPT || margin >= CCST_MATCH_MARGIN)) {
#if CCST_PERSIST_DEBUG
		CCST_Persist_Debug("&ePersist dbg: ACCEPT (threshold/margin passed)");
#endif
		return true;
	}
#if CCST_PERSIST_DEBUG
	if (ctx.best > CCST_MATCH_THRESHOLD) CCST_Persist_Debug("&ePersist dbg: REJECT reason=best>threshold");
	else CCST_Persist_Debug("&ePersist dbg: REJECT reason=margin");
#endif
	return false;
}

static void CCST_Persist_TryRestorePosition(const struct CCST_PersistFile* f) {
	struct LocalPlayer* p = Entities.CurPlayer;
	if (!p) return;
	if (!p->Hacks.CanRespawn) {
		if (!ccst_persist_posDeniedWarned) {
			ccst_persist_posDeniedWarned = true;
			CCST_Chat("&eRestored save data, but last position could not be restored (server disallows respawn).");
		}
		return;
	}

	p->Base.Position.x = f->px;
	p->Base.Position.y = f->py;
	p->Base.Position.z = f->pz;
	p->Base.Yaw = f->yaw;
	p->Base.Pitch = f->pitch;
	p->Base.Velocity.x = 0.0f;
	p->Base.Velocity.y = 0.0f;
	p->Base.Velocity.z = 0.0f;
	p->Base.OnGround = true;
	p->OldVelocity = p->Base.Velocity;
	CCST_Health_SuppressFallDamage(10);
	p->Base.prev.pos   = p->Base.Position;
	p->Base.next.pos   = p->Base.Position;
	p->Base.prev.yaw   = p->Base.Yaw;
	p->Base.next.yaw   = p->Base.Yaw;
	p->Base.prev.pitch = p->Base.Pitch;
	p->Base.next.pitch = p->Base.Pitch;
}

void CCST_Persist_OnNewMapLoaded(void) {
	if (!CCST_Policy_PluginEnabled()) return;
	if (!CCST_Policy_IsGamemodeForced()) {
		CCST_Health_SetModeSurvival(ccst_opt_default_survival);
	}
	if (!ccst_opt_firsttime_seen) {
		CCST_Chat("&9================================================");
		CCST_Chat("&b CC:ST initialized!");
		CCST_Chat("&7 If you want to change the default gamemode, ");
		CCST_Chat("&7 edit the defaults in the new &dccst/options.cfg file.");
		CCST_Chat("&e Defaults: &7gamemode=creative, autosave=on.");
		CCST_Chat("&7 Try &f/client gamemode survival || creative&7!");
		CCST_Chat("&9================================================");
		ccst_opt_firsttime_seen = true;
		CCST_Persist_SaveOptions();
	}
	ccst_persist_restorePending   = true;
	ccst_persist_restoreDelayTicks = CCST_RESTORE_MIN_DELAY_TICKS;
	ccst_persist_restoreTryTicks   = CCST_RESTORE_MAX_WAIT_TICKS;
	ccst_persist_restoreStableTicks = 0;
	ccst_persist_prevWorldFingerprint = 0;
	ccst_persist_prevFingerprintValid = false;
	ccst_persist_restoreAttempted = false;
	ccst_persist_saveAcc           = 0;
	ccst_persist_savedNoMatch      = false;
	ccst_persist_hasActivePath     = false;
	ccst_persist_activePath[0]     = '\0';
	ccst_persist_hasSuggestedCandidate = false;
	ccst_persist_suggestedPath[0] = '\0';
	ccst_persist_lastBestScore     = FLT_MAX;
	ccst_persist_lastSecondScore   = FLT_MAX;
	ccst_persist_debugLastStableLogged = -1;
	ccst_persist_debugTicksSinceLog = 0;
	ccst_persist_needsDescriptor   = true;
#if CCST_PERSIST_DEBUG
	Chat_Add3("&ePersist dbg: map load, delay=%i maxWait=%i stableNeed=%i", &ccst_persist_restoreDelayTicks, &ccst_persist_restoreTryTicks, &(int){CCST_RESTORE_STABLE_TICKS});
#endif
}

static void CCST_Persist_OnDisconnected(void* obj) {
	(void)obj;
	if (ccst_opt_autosave) CCST_Persist_SaveNow();
}

void CCST_Persist_Init(void) {
	CCST_Persist_LoadOptions();
	ccst_persist_posDeniedWarned = false;
	ccst_persist_saveAcc         = 0;
	ccst_persist_restorePending  = false;
	ccst_persist_restoreDelayTicks = 0;
	ccst_persist_restoreTryTicks   = 0;
	ccst_persist_restoreStableTicks = 0;
	ccst_persist_prevWorldFingerprint = 0;
	ccst_persist_prevFingerprintValid = false;
	ccst_persist_restoreAttempted = false;
	ccst_persist_savedNoMatch      = false;
	ccst_persist_hasActivePath     = false;
	ccst_persist_activePath[0]     = '\0';
	ccst_persist_hasSuggestedCandidate = false;
	ccst_persist_suggestedPath[0] = '\0';
	ccst_persist_lastBestScore     = FLT_MAX;
	ccst_persist_lastSecondScore   = FLT_MAX;
	ccst_persist_debugLastStableLogged = -1;
	ccst_persist_debugTicksSinceLog = 0;
	Event_Register_(&NetEvents.Disconnected, NULL, CCST_Persist_OnDisconnected);
}

void CCST_Persist_Free(void) {
	Event_Unregister_(&NetEvents.Disconnected, NULL, CCST_Persist_OnDisconnected);
}

void CCST_Persist_OnTick(struct ScheduledTask* task) {
	struct CCST_PersistFile f;
	(void)task;

	if (ccst_persist_restorePending) {
		if (ccst_persist_restoreDelayTicks > 0) ccst_persist_restoreDelayTicks--;
		else if (World.Loaded && World.Blocks && !Server.Disconnected && Entities.CurPlayer) {
			cc_uint64 fp = CCST_Persist_QuickWorldFingerprint();
			if (ccst_persist_prevFingerprintValid && fp == ccst_persist_prevWorldFingerprint)
				ccst_persist_restoreStableTicks++;
			else
				ccst_persist_restoreStableTicks = 0;
			ccst_persist_prevWorldFingerprint = fp;
			ccst_persist_prevFingerprintValid = true;
			ccst_persist_debugTicksSinceLog++;
#if CCST_PERSIST_DEBUG
			if (ccst_persist_restoreStableTicks != ccst_persist_debugLastStableLogged || ccst_persist_debugTicksSinceLog >= 20) {
				ccst_persist_debugLastStableLogged = ccst_persist_restoreStableTicks;
				ccst_persist_debugTicksSinceLog = 0;
				Chat_Add3("&ePersist dbg: stable=%i/%i waitLeft=%i", &ccst_persist_restoreStableTicks, &(int){CCST_RESTORE_STABLE_TICKS}, &ccst_persist_restoreTryTicks);
			}
#endif

			if (ccst_persist_restoreTryTicks > 0) ccst_persist_restoreTryTicks--;

			if (!ccst_persist_restoreAttempted &&
				(ccst_persist_restoreStableTicks >= CCST_RESTORE_STABLE_TICKS || ccst_persist_restoreTryTicks <= 0)) {
				ccst_persist_restoreAttempted = true;
#if CCST_PERSIST_DEBUG
				CCST_Persist_Debug("&ePersist dbg: attempting descriptor compute + match now");
#endif
				ccst_persist_hasSuggestedCandidate = false;
				ccst_persist_suggestedPath[0] = '\0';
				if (CCST_Persist_ComputeDescriptor(&ccst_persist_cachedDesc)) {
					ccst_persist_needsDescriptor = false;
					if (CCST_Persist_FindBestCandidate(&f, ccst_persist_activePath, sizeof(ccst_persist_activePath), &ccst_persist_cachedDesc)) {
						ccst_persist_restorePending = false;
						ccst_persist_hasActivePath = true;
						ccst_persist_posDeniedWarned = (f.flags & CCST_PERSIST_FLAG_POS_DENIED_WARNED) != 0;
						CCST_Persist_TryRestoreGamemode(&f);
						CCST_Inv_SetState(f.blocks, f.counts, (BlockID)f.transmuteBlock, (int)f.transmuteCount);
						CCST_Health_Set((int)f.health);
						CCST_Score_Set((int)f.score);
						CCST_Persist_TryRestorePosition(&f);
						CCST_Chat("&eRestored save data!");
						return;
					}
				}

				/* Save "possible" candidate for optional force restore command. */
				if (ccst_persist_lastBestScore < FLT_MAX && ccst_persist_lastBestScore <= CCST_MATCH_POSSIBLE_THRESHOLD) {
					ccst_persist_hasSuggestedCandidate = true;
					memcpy(&ccst_persist_suggestedCandidate, &f, sizeof(f));
					if (ccst_persist_activePath[0]) {
						cc_string p = String_FromReadonly(ccst_persist_activePath);
						(void)CCST_CopyToRaw(ccst_persist_suggestedPath, sizeof(ccst_persist_suggestedPath), &p);
					}
					CCST_Chat("&ePossible candidate save found, but no confident match.");
					CCST_Chat("&eUse &f/client ccst force &eto load the most likely candidate. &4(might damage existing saves!!)");
				}
#if CCST_PERSIST_DEBUG
				CCST_Persist_Debug("&ePersist dbg: attempt complete, no accepted match");
#endif
			}

			if (ccst_persist_restoreAttempted) {
				if (!ccst_persist_savedNoMatch) {
					if (ccst_opt_autosave) CCST_Persist_SaveNow();
					ccst_persist_savedNoMatch = true;
				}
				ccst_persist_restorePending = false;
			}
		}
	}

	ccst_persist_saveAcc++;
	if (ccst_opt_autosave && ccst_persist_saveAcc >= CCST_SAVE_INTERVAL_TICKS) {
		ccst_persist_saveAcc = 0;
		CCST_Persist_SaveNow();
	}
}

cc_bool CCST_Persist_HasSuggestedCandidate(void) {
	return ccst_persist_hasSuggestedCandidate;
}

cc_bool CCST_Persist_ForceSuggestedRestore(void) {
	if (!ccst_persist_hasSuggestedCandidate) return false;
	if (!World.Loaded || !Entities.CurPlayer) return false;

	CCST_Persist_TryRestoreGamemode(&ccst_persist_suggestedCandidate);
	CCST_Inv_SetState(ccst_persist_suggestedCandidate.blocks, ccst_persist_suggestedCandidate.counts,
		(BlockID)ccst_persist_suggestedCandidate.transmuteBlock, (int)ccst_persist_suggestedCandidate.transmuteCount);
	CCST_Health_Set((int)ccst_persist_suggestedCandidate.health);
	CCST_Score_Set((int)ccst_persist_suggestedCandidate.score);
	CCST_Persist_TryRestorePosition(&ccst_persist_suggestedCandidate);

	if (ccst_persist_suggestedPath[0]) {
		cc_string p = String_FromReadonly(ccst_persist_suggestedPath);
		(void)CCST_CopyToRaw(ccst_persist_activePath, sizeof(ccst_persist_activePath), &p);
		ccst_persist_hasActivePath = true;
	}
	ccst_persist_hasSuggestedCandidate = false;
	ccst_persist_suggestedPath[0] = '\0';
	return true;
}

cc_bool CCST_Persist_ResetCurrentWorldSave(void) {
	if (!World.Loaded || !World.Blocks || Server.Disconnected) return false;
	/* Reset should update the currently selected save slot when possible.
	 * If no active slot exists yet, but a suggested candidate exists, bind reset to that slot. */
	if (!ccst_persist_hasActivePath && ccst_persist_suggestedPath[0]) {
		cc_string s = String_FromReadonly(ccst_persist_suggestedPath);
		(void)CCST_CopyToRaw(ccst_persist_activePath, sizeof(ccst_persist_activePath), &s);
		ccst_persist_hasActivePath = true;
	}
	ccst_persist_hasSuggestedCandidate = false;
	ccst_persist_suggestedPath[0] = '\0';
	return CCST_Persist_SaveNow();
}
