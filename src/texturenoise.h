#ifndef CCST_TEXTURENOISE_H
#define CCST_TEXTURENOISE_H

#include "Core.h"

/* Samples terrain.png tiles in Atlas2D.Bmp (Tenengrad + Laplacian + local contrast on luma) for a
 * high-frequency score; smoother tiles -> higher mass (worth less when HF-rich -> craft more per input). */
void CCST_TextureNoise_Init(void);
void CCST_TextureNoise_Free(void);

/* Milli multiplier applied on top of collision volume (typ ~880..1120 when atlas CPU bitmap is valid; else 1000). */
int CCST_TextureNoise_WeightMilliForBlock(BlockID b);
/* Mean tile color -> HSV; hueDegrees 0..360 or -1 if unknown; sat 0..1. */
void CCST_TextureNoise_MeanHueSatForBlock(BlockID b, float* hueDegrees, float* sat);

#endif
