/*
XGPU.H

What nv2a_vsh_cpu.c and nv2a_combiner_ff.c (copied unchanged from
halo-ce-universal's port/linux/src, where they serve its OpenGL 1.1
renderer) take from that renderer's xgpu.h: the Xbox SDK's types and the
NV2A's vertex register counts.
*/

#ifndef __HALO_LINUX_XGPU_H
#define __HALO_LINUX_XGPU_H

#include "platform.h"

/* vertex attribute registers, and constants c[-96]..c[95] */
#define XGPU_VERTEX_ATTRIBUTE_COUNT 16
#define XGPU_VERTEX_CONSTANT_COUNT 192
#define XGPU_VERTEX_CONSTANT_BIAS 96

/* what a texture stage samples (nv2a_ff_state's sampler_type) */
enum
{
	_xgpu_sampler_none = 0,
	_xgpu_sampler_2d,
	_xgpu_sampler_3d,
	_xgpu_sampler_cube
};

#endif
