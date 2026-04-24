/*
    blockvalue.c
    block valuation and scoring.

    - visual and physical "stuff" scoring
    - hue-rarity and luxury factor calculations
    - recomputes on atlas or block definition changes

	ill be honest i gave AI a summary of what i wanted this to do 
	(noisy textures are cheaper (stone > cobblestone), logs more expensive than planks, slabs = 1/2 full, etc...)
	outputs look reasonable so im calling it good enough
*/
#include "blockvalue.h"
#include "Audio.h"
#include "Bitmap.h"
#include "Block.h"
#include "Constants.h"
#include "Event.h"
#include "Funcs.h"
#include "TexturePack.h"
#include <math.h>
#include <string.h>

#define CCST_TEX_CACHE_SIZE 512
#define CCST_HUE_BINS 18 

#define CCST_VALUE_MIN_STUFF 0.001f

#define CCST_ALPHA_MIN 128
#define CCST_TILE_VISIBLE_MIN_FRAC 0.05f
#define CCST_HUE_SAT_MIN_MI 300
#define CCST_HUE_VAL_MIN_MI 150

static cc_bool s_registered;
static cc_bool s_have_atlas;

// per-tile feature arrays
static cc_uint16 s_tile_sat_mi[CCST_TEX_CACHE_SIZE];    
static cc_uint16 s_tile_val_mi[CCST_TEX_CACHE_SIZE];    
static cc_uint16 s_tile_hue_deg[CCST_TEX_CACHE_SIZE];   
static cc_uint16 s_tile_chroma_mi[CCST_TEX_CACHE_SIZE]; 
static cc_uint16 s_tile_hf_mi[CCST_TEX_CACHE_SIZE];     
static cc_uint16 s_tile_visible_mi[CCST_TEX_CACHE_SIZE];

// atlas-normalized precomputed rarity
static float s_hue_rarity_mul[CCST_HUE_BINS]; 

// per-block cached outputs
static float s_block_stuff[BLOCK_COUNT];         
static cc_uint16 s_block_luxury_mi[BLOCK_COUNT]; 
static cc_int16  s_block_hue_sin_mi[BLOCK_COUNT]; 
static cc_int16  s_block_hue_cos_mi[BLOCK_COUNT]; 
static cc_uint16 s_block_hue_sat_mi[BLOCK_COUNT]; 

// helpers
static int CCST_Luma(int r, int g, int b) {
	return (77 * r + 150 * g + 29 * b) >> 8;
}

static float CCST_Clamp01(float x) {
	if (x < 0.0f) return 0.0f;
	if (x > 1.0f) return 1.0f;
	return x;
}

static void CCST_RgbToHsv_Mi(int r, int g, int bb, int* satMi, int* valMi, int* hueDeg) {
	int mx, mn, d;
	float rf, gf, bbf, mxf, mnf, df, hh;

	mx = r; if (g > mx) mx = g; if (bb > mx) mx = bb;
	mn = r; if (g < mn) mn = g; if (bb < mn) mn = bb;
	d = mx - mn;
	if (mx <= 0) {
		*satMi = 0; *valMi = 0; *hueDeg = 0xFFFF;
		return;
	}
	*valMi = (mx * 1000) / 255;
	*satMi = (d * 1000) / mx;
	if (d < 4) { *hueDeg = 0xFFFF; return; }

	rf = (float)r / 255.0f;
	gf = (float)g / 255.0f;
	bbf = (float)bb / 255.0f;
	mxf = (float)mx / 255.0f;
	mnf = (float)mn / 255.0f;
	df = mxf - mnf;

	if (mx == r)      hh = (gf - bbf) / df + (gf < bbf ? 6.0f : 0.0f);
	else if (mx == g) hh = (bbf - rf) / df + 2.0f;
	else              hh = (rf - gf) / df + 4.0f;
	hh *= 60.0f;
	if (hh < 0.0f)   hh += 360.0f;
	if (hh >= 360.0f) hh -= 360.0f;
	*hueDeg = (int)(hh + 0.5f);
	if (*hueDeg >= 360) *hueDeg = 359;
}


static void CCST_TileSample(struct Bitmap* bmp, int ox, int oy, int size,
	int* meanR, int* meanG, int* meanB,
	cc_uint64* chromaRaw,
	cc_uint64* noiseRaw, int* noiseN,
	float* visibleFrac) {
	cc_uint64 sR, sG, sB;
	cc_uint64 chromaAcc;
	int opaqueN;
	int totalN;
	int x, y;
	BitmapCol c;
	int mr, mg, mb;
	int p00, p01, p02, p10, p11, p12, p20, p21, p22;
	int gx, gy, la;
	cc_uint64 noiseAcc;
	int noiseCount;

	sR = sG = sB = 0;
	opaqueN = 0;
	totalN = size * size;
	for (y = 0; y < size; y++) {
		for (x = 0; x < size; x++) {
			c = Bitmap_GetPixel(bmp, ox + x, oy + y);
			if ((int)BitmapCol_A(c) < CCST_ALPHA_MIN) continue;
			sR += BitmapCol_R(c);
			sG += BitmapCol_G(c);
			sB += BitmapCol_B(c);
			opaqueN++;
		}
	}
	*visibleFrac = totalN > 0 ? (float)opaqueN / (float)totalN : 0.0f;
	if (opaqueN <= 0) {
		*meanR = *meanG = *meanB = 0;
		*chromaRaw = 0;
		*noiseRaw = 0;
		*noiseN = 0;
		return;
	}
	mr = (int)(sR / (cc_uint64)opaqueN);
	mg = (int)(sG / (cc_uint64)opaqueN);
	mb = (int)(sB / (cc_uint64)opaqueN);
	*meanR = mr; *meanG = mg; *meanB = mb;

	chromaAcc = 0;
	for (y = 0; y < size; y++) {
		for (x = 0; x < size; x++) {
			int dr, dg, db;
			c = Bitmap_GetPixel(bmp, ox + x, oy + y);
			if ((int)BitmapCol_A(c) < CCST_ALPHA_MIN) continue;
			dr = BitmapCol_R(c) - mr; if (dr < 0) dr = -dr;
			dg = BitmapCol_G(c) - mg; if (dg < 0) dg = -dg;
			db = BitmapCol_B(c) - mb; if (db < 0) db = -db;
			chromaAcc += (cc_uint64)(dr + dg + db);
		}
	}
	*chromaRaw = chromaAcc / (cc_uint64)opaqueN;

	noiseAcc = 0;
	noiseCount = 0;
	if (size >= 3) {
		for (y = 1; y < size - 1; y++) {
			for (x = 1; x < size - 1; x++) {
				BitmapCol n00, n01, n02, n10, n11, n12, n20, n21, n22;
				int skip;
				int tenen;
				n00 = Bitmap_GetPixel(bmp, ox + x - 1, oy + y - 1);
				n01 = Bitmap_GetPixel(bmp, ox + x,     oy + y - 1);
				n02 = Bitmap_GetPixel(bmp, ox + x + 1, oy + y - 1);
				n10 = Bitmap_GetPixel(bmp, ox + x - 1, oy + y    );
				n11 = Bitmap_GetPixel(bmp, ox + x,     oy + y    );
				n12 = Bitmap_GetPixel(bmp, ox + x + 1, oy + y    );
				n20 = Bitmap_GetPixel(bmp, ox + x - 1, oy + y + 1);
				n21 = Bitmap_GetPixel(bmp, ox + x,     oy + y + 1);
				n22 = Bitmap_GetPixel(bmp, ox + x + 1, oy + y + 1);
				skip = ((int)BitmapCol_A(n00) < CCST_ALPHA_MIN)
				     | ((int)BitmapCol_A(n01) < CCST_ALPHA_MIN)
				     | ((int)BitmapCol_A(n02) < CCST_ALPHA_MIN)
				     | ((int)BitmapCol_A(n10) < CCST_ALPHA_MIN)
				     | ((int)BitmapCol_A(n11) < CCST_ALPHA_MIN)
				     | ((int)BitmapCol_A(n12) < CCST_ALPHA_MIN)
				     | ((int)BitmapCol_A(n20) < CCST_ALPHA_MIN)
				     | ((int)BitmapCol_A(n21) < CCST_ALPHA_MIN)
				     | ((int)BitmapCol_A(n22) < CCST_ALPHA_MIN);
				if (skip) continue;
				p00 = CCST_Luma(BitmapCol_R(n00), BitmapCol_G(n00), BitmapCol_B(n00));
				p01 = CCST_Luma(BitmapCol_R(n01), BitmapCol_G(n01), BitmapCol_B(n01));
				p02 = CCST_Luma(BitmapCol_R(n02), BitmapCol_G(n02), BitmapCol_B(n02));
				p10 = CCST_Luma(BitmapCol_R(n10), BitmapCol_G(n10), BitmapCol_B(n10));
				p11 = CCST_Luma(BitmapCol_R(n11), BitmapCol_G(n11), BitmapCol_B(n11));
				p12 = CCST_Luma(BitmapCol_R(n12), BitmapCol_G(n12), BitmapCol_B(n12));
				p20 = CCST_Luma(BitmapCol_R(n20), BitmapCol_G(n20), BitmapCol_B(n20));
				p21 = CCST_Luma(BitmapCol_R(n21), BitmapCol_G(n21), BitmapCol_B(n21));
				p22 = CCST_Luma(BitmapCol_R(n22), BitmapCol_G(n22), BitmapCol_B(n22));
				gx = -p00 + p02 - (p10 << 1) + (p12 << 1) - p20 + p22;
				gy = -p00 - (p01 << 1) - p02 + p20 + (p21 << 1) + p22;
				tenen = (gx * gx + gy * gy) >> 10;
				la = 4 * p11 - p01 - p10 - p12 - p21;
				if (la < 0) la = -la;
				noiseAcc += (cc_uint64)(tenen + la * 2);
				noiseCount++;
			}
		}
	}
	*noiseRaw = noiseAcc;
	*noiseN = noiseCount;
}

static void CCST_BlockValue_RecomputeTileFeatures(void) {
	int tx, ty, size, rows, maxTi;
	int mr, mg, mb, satMi, valMi, hueDeg;
	cc_uint64 chromaRaw, noiseRaw;
	int noiseN;
	float visFrac;
	cc_uint32 chroma_arr[CCST_TEX_CACHE_SIZE];
	cc_uint32 noise_arr[CCST_TEX_CACHE_SIZE];
	int hueBinCount[CCST_HUE_BINS];
	float hueBinFrac[CCST_HUE_BINS];
	cc_uint32 emin, emax;
	int totalSatTiles;
	int i;
	float maxFrac;

	s_have_atlas = (Atlas2D.Bmp.scan0 != NULL) && (Atlas2D.TileSize >= 3);
	if (!s_have_atlas) {
		for (i = 0; i < CCST_TEX_CACHE_SIZE; i++) {
			s_tile_sat_mi[i] = 150;
			s_tile_val_mi[i] = 500;
			s_tile_hue_deg[i] = 0xFFFF;
			s_tile_chroma_mi[i] = 300;
			s_tile_hf_mi[i] = 500;
			s_tile_visible_mi[i] = 1000;
		}
		for (i = 0; i < CCST_HUE_BINS; i++) s_hue_rarity_mul[i] = 0.5f;
		return;
	}

	size = Atlas2D.TileSize;
	rows = Atlas2D.RowsCount;
	if (rows < 1) rows = 1;
	maxTi = rows * ATLAS2D_TILES_PER_ROW;
	if (maxTi > CCST_TEX_CACHE_SIZE) maxTi = CCST_TEX_CACHE_SIZE;

	for (i = 0; i < CCST_TEX_CACHE_SIZE; i++) {
		s_tile_sat_mi[i] = 0;
		s_tile_val_mi[i] = 0;
		s_tile_hue_deg[i] = 0xFFFF;
		s_tile_visible_mi[i] = 0;
		chroma_arr[i] = 0;
		noise_arr[i] = 0;
	}
	for (i = 0; i < CCST_HUE_BINS; i++) hueBinCount[i] = 0;
	totalSatTiles = 0;

	for (ty = 0; ty < rows; ty++) {
		for (tx = 0; tx < ATLAS2D_TILES_PER_ROW; tx++) {
			int idx = ty * ATLAS2D_TILES_PER_ROW + tx;
			if (idx >= maxTi) break;
			CCST_TileSample(&Atlas2D.Bmp, tx * size, ty * size, size,
				&mr, &mg, &mb, &chromaRaw, &noiseRaw, &noiseN, &visFrac);
			s_tile_visible_mi[idx] = (cc_uint16)(visFrac * 1000.0f + 0.5f);
			if (visFrac < CCST_TILE_VISIBLE_MIN_FRAC) {
				continue;
			}
			CCST_RgbToHsv_Mi(mr, mg, mb, &satMi, &valMi, &hueDeg);
			s_tile_sat_mi[idx] = (cc_uint16)satMi;
			s_tile_val_mi[idx] = (cc_uint16)valMi;
			s_tile_hue_deg[idx] = (cc_uint16)hueDeg;
			chroma_arr[idx] = (cc_uint32)chromaRaw;
			noise_arr[idx] = (cc_uint32)(noiseN > 0 ? (noiseRaw / (cc_uint64)noiseN) : 0);

			if (hueDeg != 0xFFFF && satMi >= CCST_HUE_SAT_MIN_MI && valMi >= CCST_HUE_VAL_MIN_MI) {
				int bin = hueDeg / (360 / CCST_HUE_BINS);
				if (bin >= CCST_HUE_BINS) bin = CCST_HUE_BINS - 1;
				hueBinCount[bin]++;
				totalSatTiles++;
			}
		}
	}

	emin = 0xFFFFFFFFu; emax = 0;
	for (i = 0; i < maxTi; i++) {
		cc_uint32 v = chroma_arr[i];
		if (s_tile_visible_mi[i] < (cc_uint16)(CCST_TILE_VISIBLE_MIN_FRAC * 1000.0f)) continue;
		if (v < emin) emin = v;
		if (v > emax) emax = v;
	}
	for (i = 0; i < CCST_TEX_CACHE_SIZE; i++) {
		if (emax > emin && i < maxTi && s_tile_visible_mi[i] > 0)
			s_tile_chroma_mi[i] = (cc_uint16)(((cc_uint64)(chroma_arr[i] > emin ? chroma_arr[i] - emin : 0) * 1000u)
				/ (emax - emin));
		else
			s_tile_chroma_mi[i] = 300;
	}

	emin = 0xFFFFFFFFu; emax = 0;
	for (i = 0; i < maxTi; i++) {
		cc_uint32 v = noise_arr[i];
		if (s_tile_visible_mi[i] < (cc_uint16)(CCST_TILE_VISIBLE_MIN_FRAC * 1000.0f)) continue;
		if (v < emin) emin = v;
		if (v > emax) emax = v;
	}
	for (i = 0; i < CCST_TEX_CACHE_SIZE; i++) {
		if (emax > emin && i < maxTi && s_tile_visible_mi[i] > 0)
			s_tile_hf_mi[i] = (cc_uint16)(((cc_uint64)(noise_arr[i] > emin ? noise_arr[i] - emin : 0) * 1000u)
				/ (emax - emin));
		else
			s_tile_hf_mi[i] = 500;
	}

	maxFrac = 0.0f;
	for (i = 0; i < CCST_HUE_BINS; i++) {
		hueBinFrac[i] = (totalSatTiles > 0)
			? (float)hueBinCount[i] / (float)totalSatTiles : 0.0f;
		if (hueBinFrac[i] > maxFrac) maxFrac = hueBinFrac[i];
	}
	for (i = 0; i < CCST_HUE_BINS; i++) {
		if (maxFrac > 0.0f) {
			float r = 1.0f - (hueBinFrac[i] / maxFrac);
			s_hue_rarity_mul[i] = CCST_Clamp01(r);
		} else {
			s_hue_rarity_mul[i] = 0.5f;
		}
	}
}


static float CCST_SoundTier_Mul(cc_uint8 s) {
	switch (s) {
	case SOUND_METAL:  return 2.00f;
	case SOUND_GLASS:  return 1.40f;
	case SOUND_STONE:  return 1.05f;
	case SOUND_WOOD:   return 0.95f;
	case SOUND_GRAVEL: return 0.65f;
	case SOUND_CLOTH:  return 0.55f;
	case SOUND_SAND:   return 0.50f;
	case SOUND_GRASS:  return 0.45f;
	case SOUND_SNOW:   return 0.35f;
	case SOUND_NONE:
	default:           return 0.30f;
	}
}

static float CCST_Draw_Mul(cc_uint8 draw) {
	switch (draw) {
	case DRAW_SPRITE:            return 0.25f;
	case DRAW_TRANSPARENT_THICK: return 0.75f;
	case DRAW_TRANSPARENT:       return 0.75f;
	case DRAW_TRANSLUCENT:       return 0.65f;
	case DRAW_OPAQUE:
	default:                     return 1.00f;
	}
}

/*
 * Multi-face multiplier. A plain 1-texture cube (cobble, stone, dirt) gets 1.0. Blocks that
 * bother to use distinct top/bottom/side textures are "crafted" or "grown", logs vs planks, 
 * pillars vs stone, bookshelf, gold/iron
 * and carry proportionally more raw material to render that complexity.
 *
 * 2 -> 3.5 is calibrated so that wood logs (2 distinct tiles, wood sound, mid-luma) score
 * ~4× wood planks (1 distinct tile, wood sound, neutral luma), matching b1.7.3's 1 log ->
 * 4 planks recipe once the per-pair rational search lands it at exact (1,4).
 *
 * 3 -> 4.3 keeps 3-face iron/gold block scoring ~9× their raw 1-face counterparts, 
 * landing the 9:1 ingot->block compression.
 */
static float CCST_DistinctFaces_Mul(int distinct) {
	if (distinct <= 1) return 1.00f;
	if (distinct == 2) return 3.50f;
	if (distinct == 3) return 4.30f;
	if (distinct == 4) return 4.80f;
	if (distinct == 5) return 5.20f;
	return 5.50f;
}

static int CCST_DistinctFaceTex(BlockID b) {
	TextureLoc seen[FACE_COUNT];
	int nSeen, distinct, f, i;
	TextureLoc t;
	cc_bool dup;

	nSeen = 0;
	distinct = 0;
	for (f = 0; f < FACE_COUNT; f++) {
		t = Block_Tex(b, f);
		dup = false;
		for (i = 0; i < nSeen; i++) {
			if (seen[i] == t) { dup = true; break; }
		}
		if (!dup) {
			seen[nSeen++] = t;
			distinct++;
		}
	}
	return distinct;
}

/*
 * Exact float collision volume in [v_floor .. 1.0]. Since per-pair ratios are computed
 * directly from stuff(from)/stuff(to), no quantization is needed: a slab (0.5) and a
 * full block (1.0) with otherwise-identical signatures yield a ratio of exactly 0.5, and
 * the best-rational search in blocktype.c nails it to 2:1.
 */
static float CCST_Block_VolFrac(BlockID b) {
	float dx, dy, dz, v;
	dx = Blocks.MaxBB[b].x - Blocks.MinBB[b].x;
	dy = Blocks.MaxBB[b].y - Blocks.MinBB[b].y;
	dz = Blocks.MaxBB[b].z - Blocks.MinBB[b].z;
	if (dx < 0) dx = -dx;
	if (dy < 0) dy = -dy;
	if (dz < 0) dz = -dz;
	if (dx > 1) dx = 1;
	if (dy > 1) dy = 1;
	if (dz > 1) dz = 1;
	v = dx * dy * dz;
	if (v < 0.0625f) v = 0.0625f; /* floor at 1/16 so sub-visible bits still have value. */
	if (v > 1.0f) v = 1.0f;
	return v;
}

/* Aggregate per-face tile features for block b (weighted by each tile's visible fraction,
   so sprite/transparent tiles with large α=0 regions don't bias the averages). */
static void CCST_BlockMeanTileFeatures(BlockID b,
	float* satMean, float* valMean, float* chromaMean, float* hfMean,
	float* hueRarityMean) {
	int f, maxTi;
	TextureLoc t;
	float wSum, s, v, ch, hf, hr;
	int vis;
	int bin;

	*satMean = 0.0f; *valMean = 0.5f; *chromaMean = 0.3f; *hfMean = 0.5f; *hueRarityMean = 0.3f;
	if (!s_have_atlas) return;

	maxTi = Atlas2D.RowsCount * ATLAS2D_TILES_PER_ROW;
	if (maxTi > CCST_TEX_CACHE_SIZE) maxTi = CCST_TEX_CACHE_SIZE;

	wSum = 0.0f; s = 0.0f; v = 0.0f; ch = 0.0f; hf = 0.0f; hr = 0.0f;
	for (f = 0; f < FACE_COUNT; f++) {
		float w;
		t = Block_Tex(b, f);
		if ((int)t >= maxTi) continue;
		vis = s_tile_visible_mi[t];
		if (vis <= 0) continue;
		w = (float)vis / 1000.0f;
		wSum += w;
		s  += w * ((float)s_tile_sat_mi[t]    / 1000.0f);
		v  += w * ((float)s_tile_val_mi[t]    / 1000.0f);
		ch += w * ((float)s_tile_chroma_mi[t] / 1000.0f);
		hf += w * ((float)s_tile_hf_mi[t]     / 1000.0f);
		if (s_tile_hue_deg[t] != 0xFFFF && s_tile_sat_mi[t] >= CCST_HUE_SAT_MIN_MI) {
			bin = (int)s_tile_hue_deg[t] / (360 / CCST_HUE_BINS);
			if (bin < 0) bin = 0;
			if (bin >= CCST_HUE_BINS) bin = CCST_HUE_BINS - 1;
			hr += w * s_hue_rarity_mul[bin];
		} else {
			/* Unsaturated tile: contributes baseline 0 rarity (common greys/browns). */
			hr += w * 0.0f;
		}
	}
	if (wSum > 0.0f) {
		*satMean      = s  / wSum;
		*valMean      = v  / wSum;
		*chromaMean   = ch / wSum;
		*hfMean       = hf / wSum;
		*hueRarityMean= hr / wSum;
	}
}

/* ---- final composition ------------------------------------------------------------ */

static float CCST_ComputeBlockStuff(BlockID b, float* outLuxury) {
	float sat, val, chroma, hf, hueRar;
	float smoothness, refinementR, refinement_mul;
	float luma_mul, rarity_mul, bright_mul, draw_mul;
	float sound_mul, face_mul, vol_frac;
	int distinctFaces;
	float stuff;
	cc_uint8 brightness;

	if (outLuxury) *outLuxury = 0.0f;

	if (b == BLOCK_AIR || Blocks.Draw[b] == DRAW_GAS) return CCST_VALUE_MIN_STUFF;

	CCST_BlockMeanTileFeatures(b, &sat, &val, &chroma, &hf, &hueRar);
	smoothness = 1.0f - hf;

	/* Refinement blends saturation (pure pigments are more "processed"), chroma (how far
	   from neutral grey), and smoothness (lack of raw texture noise). Iron/gold/diamond
	   storage blocks score high; dirt/gravel/cobble score low. */
	refinementR = (sat + chroma + smoothness) / 3.0f;
	refinement_mul = 0.65f + 1.00f * CCST_Clamp01(refinementR); /* 0.65 .. 1.65 */

	/* Extreme luminance bonus, rewards very dark (obsidian) or very bright (snow/diamond)
	   materials that fall outside the typical midtone distribution of natural terrain. */
	{
		float E = fabsf(val - 0.55f) * 2.0f;
		if (E > 1.0f) E = 1.0f;
		luma_mul = 1.0f + 0.30f * E; /* 1.00 .. 1.30 */
	}

	/* Hue rarity: atlas-local 18-bin histogram. Common foliage greens / dirt browns get
	   ~0 rarity, lonely saturated hues (diamond cyan, gold yellow, lapis blue) get ~1. */
	rarity_mul = 1.0f + 0.55f * CCST_Clamp01(hueRar); /* 1.00 .. 1.55 */

	/* Light emitters are almost always valuable (torches, glowstone, lava, pumpkin lantern,
	   magma, fire). Protocol allows CPE blocks to declare Brightness 0..15. */
	brightness = Blocks.Brightness[b];
	if (brightness > 0) {
		float bmul = 1.0f + 0.06f * (float)brightness;
		if (bmul > 1.45f) bmul = 1.45f;
		bright_mul = bmul;
	} else {
		bright_mul = 1.0f;
	}

	sound_mul = CCST_SoundTier_Mul(Blocks.DigSounds[b]);
	/* Step sound can lift blocks that walk metallic without dig-sounding metallic (rare in
	   classic but possible in CPE): weighted small so it never dominates dig. */
	{
		float step_mul = CCST_SoundTier_Mul(Blocks.StepSounds[b]);
		sound_mul = 0.80f * sound_mul + 0.20f * step_mul;
	}

	distinctFaces = CCST_DistinctFaceTex(b);
	face_mul = CCST_DistinctFaces_Mul(distinctFaces);

	draw_mul = CCST_Draw_Mul(Blocks.Draw[b]);
	vol_frac = CCST_Block_VolFrac(b);

	stuff = face_mul * sound_mul * refinement_mul * luma_mul
	      * rarity_mul * bright_mul * draw_mul * vol_frac;
	if (stuff < CCST_VALUE_MIN_STUFF) stuff = CCST_VALUE_MIN_STUFF;

	if (outLuxury) {
		/* Luxury = log-scaled composite independent of volume (so slabs don't score worse
		   on discovery than their full-cube kin). Maps stuff-without-volume ≈ [0.3..30]
		   into 0..1 roughly. */
		float base = stuff / vol_frac;
		float l = logf(base > 0.1f ? base : 0.1f) / logf(30.0f);
		*outLuxury = CCST_Clamp01((l + 0.3f) / 1.3f);
	}
	return stuff;
}

/*
 * Compute visibility-weighted circular mean hue and mean saturation for block b.
 * Only tiles with saturation >= CCST_HUE_SAT_MIN_MI contribute to the hue average; tiles
 * below that threshold are achromatic and would just drag the circular mean toward wherever
 * the grey "hue" happens to land in atan2 space. Saturation mean uses all visible tiles.
 */
static void CCST_BlockComputeHueSat(BlockID b,
	float* out_sin, float* out_cos, float* out_sat) {
	int f, maxTi;
	TextureLoc t;
	float wSum_hue, wSum_sat, sinAcc, cosAcc, satAcc;
	float hue_rad, w, vis_f;
	int vis;

	*out_sin = 0.0f; *out_cos = 1.0f; *out_sat = 0.0f;
	if (!s_have_atlas) return;

	maxTi = Atlas2D.RowsCount * ATLAS2D_TILES_PER_ROW;
	if (maxTi > CCST_TEX_CACHE_SIZE) maxTi = CCST_TEX_CACHE_SIZE;

	wSum_hue = 0.0f; wSum_sat = 0.0f;
	sinAcc = 0.0f; cosAcc = 0.0f; satAcc = 0.0f;

	for (f = 0; f < FACE_COUNT; f++) {
		t = Block_Tex(b, f);
		if ((int)t >= maxTi) continue;
		vis = s_tile_visible_mi[t];
		if (vis <= 0) continue;
		vis_f = (float)vis / 1000.0f;

		/* Saturation mean: all visible tiles, weighted by visible fraction. */
		w = vis_f;
		wSum_sat += w;
		satAcc += w * ((float)s_tile_sat_mi[t] / 1000.0f);

		/* Hue circular mean: only saturated tiles, weighted by vis × sat so pale tiles
		   don't drag the mean toward their noisy hue estimate. */
		if (s_tile_hue_deg[t] != 0xFFFF && s_tile_sat_mi[t] >= CCST_HUE_SAT_MIN_MI) {
			w = vis_f * ((float)s_tile_sat_mi[t] / 1000.0f);
			hue_rad = (float)s_tile_hue_deg[t] * (3.14159265f / 180.0f);
			sinAcc += w * sinf(hue_rad);
			cosAcc += w * cosf(hue_rad);
			wSum_hue += w;
		}
	}

	if (wSum_sat > 0.0f) *out_sat = satAcc / wSum_sat;
	if (wSum_hue > 0.0f) {
		*out_sin = sinAcc / wSum_hue;
		*out_cos = cosAcc / wSum_hue;
	}
}

static void CCST_BlockValue_RecomputeScores(void) {
	int i;
	for (i = 0; i < BLOCK_COUNT; i++) {
		BlockID b = (BlockID)i;
		float lux = 0.0f;
		float stuff;
		float hsin, hcos, hsat;
		if (b == BLOCK_AIR || Blocks.Draw[b] == DRAW_GAS) {
			s_block_stuff[i] = CCST_VALUE_MIN_STUFF;
			s_block_luxury_mi[i] = 0;
			s_block_hue_sin_mi[i] = 0;
			s_block_hue_cos_mi[i] = 1000;
			s_block_hue_sat_mi[i] = 0;
			continue;
		}
		stuff = CCST_ComputeBlockStuff(b, &lux);
		s_block_stuff[i] = stuff;
		s_block_luxury_mi[i] = (cc_uint16)(lux * 1000.0f + 0.5f);

		CCST_BlockComputeHueSat(b, &hsin, &hcos, &hsat);
		{
			int hs = (int)(hsin * 1000.0f + 0.5f);
			int hc = (int)(hcos * 1000.0f + 0.5f);
			if (hs >  1000) hs =  1000;
			if (hs < -1000) hs = -1000;
			if (hc >  1000) hc =  1000;
			if (hc < -1000) hc = -1000;
			s_block_hue_sin_mi[i] = (cc_int16)hs;
			s_block_hue_cos_mi[i] = (cc_int16)hc;
		}
		s_block_hue_sat_mi[i] = (cc_uint16)(hsat * 1000.0f + 0.5f);
	}
}

static void CCST_BlockValue_RecomputeAll(void) {
	CCST_BlockValue_RecomputeTileFeatures();
	CCST_BlockValue_RecomputeScores();
}

static void CCST_OnAtlasChanged(void* obj) {
	(void)obj;
	CCST_BlockValue_RecomputeAll();
}

static void CCST_OnBlockDefChanged(void* obj) {
	(void)obj;
	CCST_BlockValue_RecomputeScores();
}

void CCST_BlockValue_Init(void) {
	int i;
	for (i = 0; i < BLOCK_COUNT; i++) {
		s_block_stuff[i] = 1.0f;
		s_block_luxury_mi[i] = 200;
		s_block_hue_sin_mi[i] = 0;
		s_block_hue_cos_mi[i] = 1000;
		s_block_hue_sat_mi[i] = 0;
	}
	for (i = 0; i < CCST_HUE_BINS; i++) s_hue_rarity_mul[i] = 0.5f;
	if (!s_registered) {
		Event_Register_(&TextureEvents.AtlasChanged,  NULL, CCST_OnAtlasChanged);
		Event_Register_(&BlockEvents.BlockDefChanged, NULL, CCST_OnBlockDefChanged);
		s_registered = true;
	}
	CCST_BlockValue_RecomputeAll();
}

void CCST_BlockValue_Free(void) {
	if (s_registered) {
		Event_Unregister_(&TextureEvents.AtlasChanged,  NULL, CCST_OnAtlasChanged);
		Event_Unregister_(&BlockEvents.BlockDefChanged, NULL, CCST_OnBlockDefChanged);
		s_registered = false;
	}
}

float CCST_BlockValue_Stuff(BlockID b) {
	float s;
	if (b >= BLOCK_COUNT) return CCST_VALUE_MIN_STUFF;
	if (b == BLOCK_AIR || Blocks.Draw[b] == DRAW_GAS) return CCST_VALUE_MIN_STUFF;
	s = s_block_stuff[b];
	if (s < CCST_VALUE_MIN_STUFF) s = CCST_VALUE_MIN_STUFF;
	return s;
}

float CCST_BlockValue_Luxury(BlockID b) {
	if (b >= BLOCK_COUNT) return 0.0f;
	if (b == BLOCK_AIR || Blocks.Draw[b] == DRAW_GAS) return 0.0f;
	return (float)s_block_luxury_mi[b] / 1000.0f;
}

/*
 * Weighted circular-mean hue (degrees 0..360) and mean saturation (0..1) for block b.
 * Blocks with saturation near zero (stone, cobble, gravel) have an effectively undefined
 * hue; callers should check *out_sat before using the hue angle.
 */
void CCST_BlockValue_MeanHueSat(BlockID b, float* out_hue_deg, float* out_sat) {
	float s, c, deg;
	if (b >= BLOCK_COUNT || b == BLOCK_AIR || Blocks.Draw[b] == DRAW_GAS) {
		*out_hue_deg = 0.0f; *out_sat = 0.0f; return;
	}
	s = (float)s_block_hue_sin_mi[b] / 1000.0f;
	c = (float)s_block_hue_cos_mi[b] / 1000.0f;
	deg = atan2f(s, c) * (180.0f / 3.14159265f);
	if (deg < 0.0f) deg += 360.0f;
	*out_hue_deg = deg;
	*out_sat = (float)s_block_hue_sat_mi[b] / 1000.0f;
}
