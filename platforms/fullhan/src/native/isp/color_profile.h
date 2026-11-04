/* Exact MIS2006 day profile; SHA256 467382145b4b7f0211755349d0a3b8f40188c7706777fbe518ef34defb17dbba.
 * Only bounded fields below0x140 are used; no stock loader overread. */
#ifndef CAMERA_COLOR_PROFILE_H
#define CAMERA_COLOR_PROFILE_H
#include <stdint.h>
#define CAMERA_AE_TARGET_Q16 11235u
#define CAMERA_COLOR_CLIP_BIAS -16
static const uint32_t camera_color_anchor[4]={0xd220534u,0xa890692u,0x8aa0895u,0x7150c61u};
static const int16_t camera_color_matrix[4][12]={
{486,26,0,-102,614,0,-76,-65,653,0,0,0},
{553,-92,51,-97,579,10,36,0,476,0,0,0},
{502,-56,66,-67,573,-10,26,-21,507,0,0,0},
{509,-58,61,-67,568,-10,51,-72,533,0,0,0},
};
static const uint8_t camera_bayer_permutation[4][4]={{0,1,3,2},{1,0,2,3},{3,2,0,1},{2,3,1,0}};
#endif
