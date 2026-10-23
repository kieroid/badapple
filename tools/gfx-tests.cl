// SPDX-License-Identifier: GPL-2.0+ OR MIT
// Copyright (c) 2026 kiki <kieran@lavato.net>
// Test kernels for tools/gfx-proxy.py clc. "make gfx-tests" compiles them with
// Mesa's asahi_clc into build/gfx/tests.c. The proxy runs each one it knows
// and checks the output.
#include "compiler/libcl/libcl.h"

KERNEL(32)
t_fill(global uint32_t *out, uint32_t value)
{
   out[cl_global_id.x] = value + cl_global_id.x;
}

KERNEL(32)
t_loop(global uint32_t *out, uint32_t n)
{
   uint id = cl_global_id.x, s = 0;
   for (uint i = 0; i < (id % n); i++)
      s += i * 3 + (i & 1 ? 7 : 1);
   out[id] = s;
}

KERNEL(64)
t_shared(global uint32_t *out, global uint32_t *in)
{
   local uint tmp[64];
   uint l = cl_local_id.x;
   tmp[l] = in[cl_global_id.x];
   barrier(CLK_LOCAL_MEM_FENCE);
   for (uint s = 32; s > 0; s >>= 1) {
      if (l < s)
         tmp[l] += tmp[l + s];
      barrier(CLK_LOCAL_MEM_FENCE);
   }
   if (l == 0)
      out[cl_group_id.x] = tmp[0];
}

KERNEL(32)
t_atomic(global uint32_t *counter)
{
   atomic_fetch_add((global atomic_uint *)counter, cl_global_id.x + 1);
}

KERNEL(32)
t_float(global float *out, float scale)
{
   float x = (float)cl_global_id.x * scale;
   out[cl_global_id.x] = x * x - 1.0f / (x + 1.0f);
}

KERNEL(64)
k_ids(global uint32_t *out)
{
   out[cl_global_id.x] = cl_local_id.x | cl_group_id.x << 16;
}

KERNEL(64)
k_barrier(global uint32_t *out)
{
   barrier(CLK_LOCAL_MEM_FENCE);
   out[cl_global_id.x] = cl_global_id.x;
}

KERNEL(64)
k_tgmem(global uint32_t *out)
{
   volatile local uint tmp[64];
   tmp[cl_local_id.x] = cl_global_id.x * 3;
   out[cl_global_id.x] = tmp[cl_local_id.x];
}

KERNEL(64)
k_tgswap(global uint32_t *out)
{
   local uint tmp[64];
   tmp[cl_local_id.x] = cl_global_id.x;
   barrier(CLK_LOCAL_MEM_FENCE);
   out[cl_global_id.x] = tmp[63 - cl_local_id.x];
}

KERNEL(64)
k_fence(global uint32_t *out)
{
   out[cl_global_id.x] = 1;
   mem_fence(CLK_GLOBAL_MEM_FENCE);
   out[cl_global_id.x] = cl_global_id.x + 7;
}

KERNEL(64)
k_atomic_noret(global uint32_t *c)
{
   atomic_fetch_add_explicit((global atomic_uint *)&c[cl_global_id.x], 5u, memory_order_relaxed);
}

KERNEL(64)
k_atomic_ret(global uint32_t *c, global uint32_t *out)
{
   out[cl_global_id.x] = atomic_fetch_add_explicit((global atomic_uint *)&c[cl_global_id.x], 5u, memory_order_relaxed);
}

KERNEL(64)
k_atomic_uniform(global uint32_t *c)
{
   atomic_fetch_add_explicit((global atomic_uint *)c, 1u, memory_order_relaxed);
}

KERNEL(64)
k_tgoff(global uint32_t *out)
{
   volatile local uint tmp[68];
   tmp[cl_local_id.x + 4] = cl_global_id.x * 3;
   out[cl_global_id.x] = tmp[cl_local_id.x + 4];
}

KERNEL(64)
k_atomic_seqcst(global uint32_t *c)
{
   atomic_fetch_add_explicit((global atomic_uint *)&c[cl_global_id.x], 5u, memory_order_seq_cst);
}

KERNEL(64)
k_simd(global uint32_t *c)
{
   atomic_fetch_add_explicit((global atomic_uint *)c, cl_global_id.x + 1, memory_order_relaxed);
}
