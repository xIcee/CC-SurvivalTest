/*
    texturenoise.c
    texture detail and color analysis.

    - high-frequency noise measurement
    - mean hue and saturation extraction
    - pack-normalized detail weighting
*/
#include "texturenoise.h"
#include "Bitmap.h"
#include "Block.h"
#include "Event.h"
#include "Funcs.h"
#include "TexturePack.h"

#define CCST_TEX_CACHE_SIZE 512

#define CCST_DETAIL_HI_MILLI 1120
#define CCST_DETAIL_LO_MILLI 880

#define CCST_HF_TENEN_SHIFT 10 
#define CCST_HF_LAP_MUL     2
#define CCST_HF_LC_MUL      1

static cc_uint16 s_tileNorm[CCST_TEX_CACHE_SIZE];
static cc_bool s_haveAtlasPixels;
static cc_bool s_registered;

static int CCST_LumaAt(struct Bitmap* bmp, int ax, int ay) {
	BitmapCol c = Bitmap_GetPixel(bmp, ax, ay);
	return (77 * BitmapCol_R(c) + 150 * BitmapCol_G(c) + 29 * BitmapCol_B(c)) >> 8;
}

static int CCST_MinInt9(int a, int b, int c, int d, int e, int f, int g, int h, int i) {
	int m = a;
	if (b < m) m = b;
	if (c < m) m = c;
	if (d < m) m = d;
	if (e < m) m = e;
	if (f < m) m = f;
	if (g < m) m = g;
	if (h < m) m = h;
	if (i < m) m = i;
	return m;
}

static int CCST_MaxInt9(int a, int b, int c, int d, int e, int f, int g, int h, int i) {
	int m = a;
	if (b > m) m = b;
	if (c > m) m = c;
	if (d > m) m = d;
	if (e > m) m = e;
	if (f > m) m = f;
	if (g > m) m = g;
	if (h > m) m = h;
	if (i > m) m = i;
	return m;
}

// compute high-frequency detail score
static cc_uint32 CCST_TileMeanHighFreq(struct Bitmap* bmp, int ox, int oy, int size) {
	int x, y, gx, gy, lap, p11;
	int lmin, lmax, lc;
	cc_uint64 acc, tenen;
	int count;

	if (size < 3) return 0;
	acc = 0;
	count = 0;
	for (y = 1; y < size - 1; y++) {
		for (x = 1; x < size - 1; x++) {
			int p00 = CCST_LumaAt(bmp, ox + x - 1, oy + y - 1);
			int p01 = CCST_LumaAt(bmp, ox + x,     oy + y - 1);
			int p02 = CCST_LumaAt(bmp, ox + x + 1, oy + y - 1);
			int p10 = CCST_LumaAt(bmp, ox + x - 1, oy + y    );
			int p12 = CCST_LumaAt(bmp, ox + x + 1, oy + y    );
			int p20 = CCST_LumaAt(bmp, ox + x - 1, oy + y + 1);
			int p21 = CCST_LumaAt(bmp, ox + x,     oy + y + 1);
			int p22 = CCST_LumaAt(bmp, ox + x + 1, oy + y + 1);

			p11 = CCST_LumaAt(bmp, ox + x, oy + y);

			gx = -p00 + p02 - (p10 << 1) + (p12 << 1) - p20 + p22;
			gy = -p00 - (p01 << 1) - p02 + p20 + (p21 << 1) + p22;
			tenen = (cc_uint64)(gx * gx + gy * gy);

			lap = 4 * p11 - p01 - p10 - p12 - p21;
			if (lap < 0) lap = -lap;

			lmin = CCST_MinInt9(p00, p01, p02, p10, p11, p12, p20, p21, p22);
			lmax = CCST_MaxInt9(p00, p01, p02, p10, p11, p12, p20, p21, p22);
			lc   = lmax - lmin;

			acc += (tenen >> CCST_HF_TENEN_SHIFT)
				+ (cc_uint64)(lap * CCST_HF_LAP_MUL)
				+ (cc_uint64)(lc * CCST_HF_LC_MUL);
			count++;
		}
	}
	if (!count) return 0;
	return (cc_uint32)(acc / (cc_uint64)count);
}

static void CCST_TextureNoise_Recompute(void) {
	int tx, ty, size, rows, maxTi;
	cc_uint32 e, emin, emax;
	cc_uint32 accum[CCST_TEX_CACHE_SIZE];
	int i;

	s_haveAtlasPixels = (Atlas2D.Bmp.scan0 != NULL) && (Atlas2D.TileSize >= 3);
	if (!s_haveAtlasPixels) {
		for (i = 0; i < CCST_TEX_CACHE_SIZE; i++) s_tileNorm[i] = 512;
		return;
	}

	size = Atlas2D.TileSize;
	rows = Atlas2D.RowsCount;
	if (rows < 1) rows = 1;
	maxTi = rows * ATLAS2D_TILES_PER_ROW;
	if (maxTi > CCST_TEX_CACHE_SIZE) maxTi = CCST_TEX_CACHE_SIZE;

	for (i = 0; i < CCST_TEX_CACHE_SIZE; i++) accum[i] = 0;

	for (ty = 0; ty < rows; ty++) {
		for (tx = 0; tx < ATLAS2D_TILES_PER_ROW; tx++) {
			int idx = ty * ATLAS2D_TILES_PER_ROW + tx;
			if (idx >= maxTi) break;
			e = CCST_TileMeanHighFreq(&Atlas2D.Bmp, tx * size, ty * size, size);
			accum[idx] = e;
		}
	}

	emin = 0xFFFFFFFFu;
	emax = 0;
	for (i = 0; i < maxTi; i++) {
		e = accum[i];
		if (e < emin) emin = e;
		if (e > emax) emax = e;
	}
	if (emin > emax) {
		emin = 0;
		emax = 0;
	}

	if (emax <= emin) {
		for (i = 0; i < CCST_TEX_CACHE_SIZE; i++) s_tileNorm[i] = 512;
		return;
	}

	for (i = 0; i < maxTi; i++) {
		e = accum[i];
		s_tileNorm[i] = (cc_uint16)((cc_uint64)(e - emin) * 1023u / (cc_uint64)(emax - emin));
	}
	for (; i < CCST_TEX_CACHE_SIZE; i++) s_tileNorm[i] = 512;
}

static void CCST_OnAtlasChanged(void* obj) {
	(void)obj;
	CCST_TextureNoise_Recompute();
}

void CCST_TextureNoise_Init(void) {
	if (!s_registered) {
		Event_Register_(&TextureEvents.AtlasChanged, NULL, CCST_OnAtlasChanged);
		s_registered = true;
	}
	CCST_TextureNoise_Recompute();
}

void CCST_TextureNoise_Free(void) {
	if (s_registered) {
		Event_Unregister_(&TextureEvents.AtlasChanged, NULL, CCST_OnAtlasChanged);
		s_registered = false;
	}
}

int CCST_TextureNoise_WeightMilliForBlock(BlockID b) {
	int f, sum, cnt, avg;
	TextureLoc t;

	if (b >= BLOCK_COUNT || b == BLOCK_AIR || Blocks.Draw[b] == DRAW_GAS) return 1000;
	if (!s_haveAtlasPixels) return 1000;

	sum = 0;
	cnt = 0;
	for (f = 0; f < FACE_COUNT; f++) {
		t = Block_Tex(b, f);
		if (t >= CCST_TEX_CACHE_SIZE) continue;
		sum += (int)s_tileNorm[t];
		cnt++;
	}
	if (cnt <= 0) return 1000;

	avg = sum / cnt;

	/* avg 0 (smoothest in pack) -> HI; avg 1023 (edgiest) -> LO */
	return CCST_DETAIL_HI_MILLI
		- (avg * (CCST_DETAIL_HI_MILLI - CCST_DETAIL_LO_MILLI)) / 1023;
}

static void CCST_AvgRgbTile(TextureLoc t, int* outR, int* outG, int* outB) {
	int tx, ty, size, ox, oy, px, py;
	cc_uint64 sr, sg, sb, n;

	tx = Atlas2D_TileX(t);
	ty = Atlas2D_TileY(t);
	size = Atlas2D.TileSize;
	ox = tx * size;
	oy = ty * size;
	sr = sg = sb = n = 0;
	for (py = 0; py < size; py++) {
		for (px = 0; px < size; px++) {
			BitmapCol c = Bitmap_GetPixel(&Atlas2D.Bmp, ox + px, oy + py);
			sr += BitmapCol_R(c);
			sg += BitmapCol_G(c);
			sb += BitmapCol_B(c);
			n++;
		}
	}
	if (!n) {
		*outR = *outG = *outB = 0;
		return;
	}
	*outR = (int)(sr / n);
	*outG = (int)(sg / n);
	*outB = (int)(sb / n);
}

static void CCST_RgbToHs(int r, int g, int b, float* hDeg, float* sat) {
	float rf, gf, bf, mx, mn, d, hh;

	rf = r / 255.0f;
	gf = g / 255.0f;
	bf = b / 255.0f;
	mx = max(rf, max(gf, bf));
	mn = min(rf, min(gf, bf));
	d  = mx - mn;
	if (mx <= 1e-5f) {
		*sat = 0.0f;
		*hDeg = -1.0f;
		return;
	}
	*sat = d / mx;
	if (d < 1e-4f) {
		*hDeg = -1.0f;
		return;
	}
	if (mx == rf)      hh = (gf - bf) / d + (gf < bf ? 6.0f : 0.0f);
	else if (mx == gf) hh = (bf - rf) / d + 2.0f;
	else               hh = (rf - gf) / d + 4.0f;
	hh *= 60.0f;
	if (hh < 0.0f) hh += 360.0f;
	if (hh >= 360.0f) hh -= 360.0f;
	*hDeg = hh;
}

void CCST_TextureNoise_MeanHueSatForBlock(BlockID blk, float* hueDegrees, float* sat) {
	int f, r, g, bb, sumR, sumG, sumB, cnt, maxTi;
	TextureLoc t;

	if (!hueDegrees || !sat) return;
	*hueDegrees = -1.0f;
	*sat = 0.0f;
	if (blk >= BLOCK_COUNT || blk == BLOCK_AIR || Blocks.Draw[blk] == DRAW_GAS) return;
	if (!s_haveAtlasPixels || Atlas2D.Bmp.scan0 == NULL || Atlas2D.TileSize < 1) return;

	maxTi = Atlas2D.RowsCount * ATLAS2D_TILES_PER_ROW;
	if (maxTi < 1) return;

	sumR = sumG = sumB = cnt = 0;
	for (f = 0; f < FACE_COUNT; f++) {
		t = Block_Tex(blk, f);
		if ((int)t >= maxTi) continue;
		CCST_AvgRgbTile(t, &r, &g, &bb);
		sumR += r;
		sumG += g;
		sumB += bb;
		cnt++;
	}
	if (cnt <= 0) return;
	r = sumR / cnt;
	g = sumG / cnt;
	bb = sumB / cnt;
	CCST_RgbToHs(r, g, bb, hueDegrees, sat);
}
