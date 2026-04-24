/*
    blocktype.c
    block behavioral properties.

    - dig/break time calculations
    - transmutation rules and ratios
    - fire variant detection
*/

#include "PluginAPI.h"
#include "Audio.h"
#include "Block.h"
#include "Core.h"
#include "ExtMath.h"
#include "Game.h"
#include "String_.h"
#include "blocktype.h"
#include "blockvalue.h"
#include <math.h>

float CCST_BlockType_BreakSecondsForDigSound(cc_uint8 digSound) {
	switch (digSound) {
	case SOUND_STONE: return 1.65f;
	case SOUND_METAL: return 3.25f;
	case SOUND_WOOD:  return 0.95f;
	case SOUND_GRAVEL:return 0.45f;
	case SOUND_GRASS: return 0.45f;
	case SOUND_CLOTH: return 0.35f;
	case SOUND_SAND:  return 0.30f;
	case SOUND_SNOW:  return 0.20f;
	case SOUND_GLASS: return 0.25f;
	case SOUND_NONE:
	default:          return 0.12f;
	}
}

float CCST_BlockType_BreakShapeMul(cc_uint8 draw) {
	switch (draw) {
	case DRAW_SPRITE:            return 0.10f;
	case DRAW_TRANSPARENT_THICK: return 0.85f;
	case DRAW_TRANSPARENT:       return 0.78f;
	case DRAW_TRANSLUCENT:       return 0.70f;
	case DRAW_OPAQUE:
	default:                     return 1.00f;
	}
}

float CCST_BlockType_BreakTimeFor(BlockID b) {
	float t, mul;
	if (b >= BLOCK_COUNT) return 0.12f;
	t   = CCST_BlockType_BreakSecondsForDigSound(Blocks.DigSounds[b]);
	mul = CCST_BlockType_BreakShapeMul(Blocks.Draw[b]);
	t *= mul;
	if (t < 0.05f) t = 0.05f;
	if (t > 12.0f) t = 12.0f;
	return t;
}

static cc_bool CCST_HasSandSound(BlockID b) {
	return Blocks.DigSounds[b] == SOUND_SAND || Blocks.StepSounds[b] == SOUND_SAND;
}
static cc_bool CCST_HasGlassSound(BlockID b) {
	return Blocks.DigSounds[b] == SOUND_GLASS || Blocks.StepSounds[b] == SOUND_GLASS;
}

static cc_bool CCST_HasClothSound(BlockID b) {
	return Blocks.DigSounds[b] == SOUND_CLOTH || Blocks.StepSounds[b] == SOUND_CLOTH;
}
// dig and/or step mapped to none, silent / unset CPE sounds; can pair with cloth like sand to glass. 
// Completes generally acceptable crafting pathways (wood > wood, stone > stone, grass > grass, sand > sand|smelt to glass, and since no sheep for wool, also unidentified > cloth).
static cc_bool CCST_MissingDigOrStepSound(BlockID b) {
	return Blocks.DigSounds[b] == SOUND_NONE || Blocks.StepSounds[b] == SOUND_NONE;
}

// true if block is "fire", since fire is wood+sprite shape but shouldn't be transmutable with other wood blocks.
static cc_bool CCST_BlockType_NameIsFireVariant(BlockID b) {
	cc_string name, suffix;
	int idx;

	name = Block_UNSAFE_GetName(b);
	if (String_CaselessEqualsConst(&name, "fire"))
		return true;

	idx = String_LastIndexOf(&name, '-');
	if (idx <= 0) return false;
	suffix = String_UNSAFE_SubstringAt(&name, idx + 1);
	if (suffix.length < 1 || suffix.length > 3) return false;
	name.length = idx;
	return String_CaselessEqualsConst(&name, "fire");
}

cc_bool CCST_BlockType_IsTransmuteExcluded(BlockID b) {
	cc_uint8 ec;

	if (b == BLOCK_AIR || b >= BLOCK_COUNT) return true;
	if (Blocks.Draw[b] == DRAW_GAS) return true;

	ec = Blocks.ExtendedCollide[b];
	if (ec == COLLIDE_WATER || ec == COLLIDE_LAVA || ec == COLLIDE_LIQUID)
		return true;
	if (Blocks.IsLiquid[b])
		return true;

	if (b == BLOCK_FIRE)
		return true;
	if (CCST_BlockType_NameIsFireVariant(b))
		return true;

	return false;
}

cc_bool CCST_BlockType_CanTransmute(BlockID from, BlockID to) {
	cc_uint8 df, ds, tf, ts;
	if (from == to) return false;
	if (from >= BLOCK_COUNT || to >= BLOCK_COUNT) return false;
	if (from == BLOCK_AIR || to == BLOCK_AIR) return false;
	if (CCST_BlockType_IsTransmuteExcluded(from) || CCST_BlockType_IsTransmuteExcluded(to))
		return false;

	df = Blocks.DigSounds[from];
	ds = Blocks.StepSounds[from];
	tf = Blocks.DigSounds[to];
	ts = Blocks.StepSounds[to];

	if (df == tf || df == ts || ds == tf || ds == ts) return true;
	if (CCST_HasSandSound(from) && CCST_HasGlassSound(to)) return true;
	if (CCST_HasGlassSound(from) && CCST_HasSandSound(to)) return true;
	if (CCST_MissingDigOrStepSound(from) && CCST_HasClothSound(to)) return true;
	if (CCST_HasClothSound(from) && CCST_MissingDigOrStepSound(to)) return true;
	return false;
}

static int CCST_GcdPositive(int a, int b) {
	if (a < 0) a = -a;
	if (b < 0) b = -b;
	while (b) {
		int t = a % b;
		a = b;
		b = t;
	}
	return a ? a : 1;
}

static void CCST_FindBestRatio(float target, int max_side, int* out_m, int* out_n) {
	float best_err;
	int best_m, best_n, best_sum;
	int m, n;
	float log_target, log_r, err;
	int g, mr, nr;
	float eps = 1e-6f;

	if (target <= 0.0f) { *out_m = 1; *out_n = 1; return; }
	if (max_side < 1) max_side = 1;

	log_target = logf(target);
	best_err = INFINITY;
	best_m = 1; best_n = 1; best_sum = 2;

	for (m = 1; m <= max_side; m++) {
		for (n = 1; n <= max_side; n++) {
			log_r = logf((float)n / (float)m);
			err = fabsf(log_r - log_target);
			if (err < best_err - eps) {
				g = CCST_GcdPositive(m, n);
				mr = m / g;
				nr = n / g;
				best_err = err;
				best_m = mr;
				best_n = nr;
				best_sum = mr + nr;
			} else if (fabsf(err - best_err) <= eps) {
				g = CCST_GcdPositive(m, n);
				mr = m / g;
				nr = n / g;
				if (mr + nr < best_sum) {
					best_m = mr;
					best_n = nr;
					best_sum = mr + nr;
				}
			}
		}
	}
	*out_m = best_m;
	*out_n = best_n;
}

cc_bool CCST_BlockType_TransmuteRatio(BlockID from, BlockID to, int* out_in, int* out_out) {
	float s_from, s_to, target;
	float hue_from, sat_from, hue_to, sat_to;
	float hdist, sat_weight, hue_penalty;
	int m, n;

	*out_in = 0;
	*out_out = 0;
	if (!CCST_BlockType_CanTransmute(from, to)) return false;

	s_from = CCST_BlockValue_Stuff(from);
	s_to   = CCST_BlockValue_Stuff(to);
	if (s_from <= 0.0f || s_to <= 0.0f) return false;

	/* When two blocks share a material class by sound but differ strongly in color 
	   (grass-green->sponge-yellow, grass-green->TNT-red), converting
	   between them costs extra */
	CCST_BlockValue_MeanHueSat(from, &hue_from, &sat_from);
	CCST_BlockValue_MeanHueSat(to,   &hue_to,   &sat_to);

	hdist = fabsf(hue_from - hue_to);
	if (hdist > 180.0f) hdist = 360.0f - hdist;
	hdist /= 180.0f; 
	sat_weight = (sat_from + sat_to) * 0.5f;
	hue_penalty = 1.0f / (1.0f + 1.5f * hdist * sat_weight);

	target = (s_from / s_to) * hue_penalty;
	CCST_FindBestRatio(target, CCST_TRANSMUTE_MAX_SIDE, &m, &n);
	*out_in = m;
	*out_out = n;
	return true;
}

void CCST_BlockType_TransmuteExchange(BlockID from, int count_from, BlockID to,
	int max_out_cap,
	int* out_produced, int* from_consumed, cc_bool* out_probabilistic) {
	int m, n, cycles_from_input, cycles_from_cap, cycles;
	int produced, consumed, remainder, room;
	float exp_out, frac;
	RNGState rnd;
	int base, extra, partial;

	if (out_probabilistic) *out_probabilistic = false;

	*out_produced = 0;
	*from_consumed = 0;
	if (count_from <= 0) return;
	if (!CCST_BlockType_TransmuteRatio(from, to, &m, &n)) return;
	if (m <= 0 || n <= 0) return;
	if (max_out_cap <= 0 || max_out_cap > CCST_TRANSMUTE_MAX_OUT)
		max_out_cap = CCST_TRANSMUTE_MAX_OUT;

	cycles_from_input = count_from / m;
	cycles_from_cap   = max_out_cap / n;
	cycles = cycles_from_input < cycles_from_cap ? cycles_from_input : cycles_from_cap;

	produced = cycles * n;
	consumed = cycles * m;
	remainder = count_from - consumed;

	/* Leftover stack smaller than one full recipe batch, but the recipe expands material (n>m) so there's a fractional expected output.
	 * Allows crafting richer materials into poorer ones even if the player doesn't have an exact mapping for convenience
	 */
	if (remainder > 0 && n > m && remainder < m) {
		room = max_out_cap - produced;
		if (room > 0) {
			exp_out = remainder * (float)n / (float)m;
			Random_Seed(&rnd, (int)(Game.Time * 1000.0) ^ ((int)from << 12) ^ ((int)to << 2)
				^ (count_from * 2654435761) ^ (remainder * 0x9E3779B1));
			base = (int)floorf(exp_out);
			frac = exp_out - (float)base;
			extra = (frac > 1e-5f && Random_Float(&rnd) < frac) ? 1 : 0;
			partial = base + extra;
			if (partial > room) partial = room;
			if (partial > 0) {
				produced += partial;
				consumed += remainder;
				if (out_probabilistic) *out_probabilistic = true;
			}
		}
	}

	*out_produced  = produced;
	*from_consumed = consumed;
}

cc_bool CCST_BlockType_TransmutePreview(BlockID from, int count_from, BlockID to,
	int max_out_cap,
	int* out_produced, int* out_consumed) {
	int m, n, cycles_from_input, cycles_from_cap, cycles;
	int produced, consumed, remainder, room, base;
	float exp_out;

	*out_produced = 0;
	*out_consumed = 0;
	if (count_from <= 0) return false;
	if (!CCST_BlockType_TransmuteRatio(from, to, &m, &n)) return false;
	if (m <= 0 || n <= 0) return false;
	if (max_out_cap <= 0 || max_out_cap > CCST_TRANSMUTE_MAX_OUT)
		max_out_cap = CCST_TRANSMUTE_MAX_OUT;

	cycles_from_input = count_from / m;
	cycles_from_cap   = max_out_cap / n;
	cycles = cycles_from_input < cycles_from_cap ? cycles_from_input : cycles_from_cap;

	produced = cycles * n;
	consumed = cycles * m;
	remainder = count_from - consumed;

	if (remainder > 0 && n > m && remainder < m) {
		room = max_out_cap - produced;
		if (room > 0) {
			exp_out = remainder * (float)n / (float)m;
			base = (int)floorf(exp_out);
			if (base > room) base = room;
			if (base > 0) {
				produced += base;
				consumed += remainder;
			} 
		}
	}

	*out_produced = produced;
	*out_consumed = consumed;
	return true;
}
