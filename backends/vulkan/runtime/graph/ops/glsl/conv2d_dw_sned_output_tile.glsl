/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#version 450 core

#define PRECISION ${PRECISION}

#define VEC4_T ${texel_type(DTYPE)}

#define TILE_SIZE ${TILE_SIZE}

#define op(X, A, B) ${OPERATOR}

#include "indexing_utils.h"

layout(std430) buffer;

${layout_declare_tensor(0, "w", "t_out", DTYPE, "texture3d")}
${layout_declare_tensor(1, "r", "t_in", DTYPE, "texture3d")}
${layout_declare_tensor(2, "r", "t_kernel", DTYPE, "texture2d")}
${layout_declare_tensor(3, "r", "t_bias", DTYPE, "texture2d")}

layout(push_constant) uniform restrict Block {
  ivec4 out_limits;
  ivec4 in_sizes;
  ivec2 kernel_size;
  ivec2 stride;
  ivec2 padding;
  ivec2 dilation;
  ivec2 overlay_region;
  int in_group_size;
  int dummy_padding;
  float out_min;
  float out_max;
};

layout(local_size_x_id = 0, local_size_y_id = 1, local_size_z_id = 2) in;

/*
 * Computes a depthwise convolution. Each shader invocation calculates the
 * output at a single output location.
 */

void main() {
  const uint div_by_x = gl_GlobalInvocationID.x / out_limits.x;
  const ivec3 pos = ivec3(
    gl_GlobalInvocationID.x % out_limits.x,
    div_by_x,
    gl_GlobalInvocationID.y);

  // do not process if top pixel does not fit within the output range
  if (pos.y >= out_limits.y || pos.z >= out_limits.z) {
    return;
  }

  // Compute the index of the top-left element of the overlay region. Negative
  // indices indicate that the top-left element is in a region added by padding.
  const ivec2 ipos = pos.xy * stride - padding;

  // Compute the start and end of the input indices to load. Padding is assumed
  // to be constant 0 padding, so any reads from the padding region is skipped.
  const ivec2 start = ipos;

  VEC4_T sum = texelFetch(t_bias, ivec2(pos.z, 0), 0);
  int kx = 0;
  for (int y = start.y, i = 0; i < TILE_SIZE; y += dilation.y, i++) {
    for (int x = start.x, j = 0; j < TILE_SIZE; x += dilation.x, j++) {
      // The weight kernel was rearranged such that every NxN filter is
      // flattened to fit in one row. Each filter was then stacked on top of
      // each other vertically.
      const vec4 in_texel = texelFetch(t_in, ivec3(x, y, pos.z), 0);
      sum = fma(in_texel, texelFetch(t_kernel, ivec2(kx, pos.z), 0), sum);
      kx++;
    }
  }

  imageStore(t_out, pos, op(sum, out_min, out_max));
}
