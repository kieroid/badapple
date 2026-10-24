// SPDX-License-Identifier: GPL-2.0+ OR MIT
// Copyright (c) 2026 kiki <kieran@lavato.net>
//
// OpenCL through Mesa's Rusticl on the A12 GPU: a few kernels covering
// arguments, floats, threadgroup memory with barriers, and global and
// threadgroup atomics, each
// checked on the CPU.

#define CL_TARGET_OPENCL_VERSION 300
#include <CL/cl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define N 4096
#define NH 65536

/* Reference conversions, exact for every half including denormals */
static float half_to_float(unsigned short h)
{
	unsigned e = h >> 10 & 0x1f, m = h & 0x3ff;
	float f = e == 0 ? m * 0x1p-24f : e == 31 ? (m ? __builtin_nanf("") : __builtin_inff())
				: (1024 + m) * __builtin_ldexpf(1.0f, (int)e - 25);

	return h & 0x8000 ? -f : f;
}

static const char *src =
	"kernel void fill(global uint *out, uint k) {\n"
	"	uint i = get_global_id(0);\n"
	"	out[i] = i + k;\n"
	"}\n"
	"kernel void saxpy(global float *y, global const float *x, float a) {\n"
	"	uint i = get_global_id(0);\n"
	"	y[i] = a * x[i] + y[i];\n"
	"}\n"
	"kernel void reverse(global uint *buf) {\n"
	"	local uint tmp[64];\n"
	"	uint l = get_local_id(0), g = get_global_id(0);\n"
	"	tmp[l] = buf[g];\n"
	"	barrier(CLK_LOCAL_MEM_FENCE);\n"
	"	buf[g] = tmp[63 - l];\n"
	"}\n"
	"kernel void count(global uint *c, global const uint *in) {\n"
	"	if (in[get_global_id(0)] & 1)\n"
	"		atomic_inc(c);\n"
	"}\n"
	"kernel void lcount(global uint *c, global const uint *in) {\n"
	"	local uint n;\n"
	"	if (get_local_id(0) == 0)\n"
	"		n = 0;\n"
	"	barrier(CLK_LOCAL_MEM_FENCE);\n"
	"	if (in[get_global_id(0)] & 1)\n"
	"		atomic_inc(&n);\n"
	"	barrier(CLK_LOCAL_MEM_FENCE);\n"
	"	if (get_local_id(0) == 0)\n"
	"		atomic_add(c, n);\n"
	"}\n"
	"kernel void h2f(global float *out, global const half *in) {\n"
	"	uint i = get_global_id(0);\n"
	"	out[i] = vload_half(i, in);\n"
	"}\n"
	"kernel void f2h(global half *out, global const float *in) {\n"
	"	uint i = get_global_id(0);\n"
	"	vstore_half(in[i], i, out);\n"
	"}\n"
	"kernel void rnd(global float4 *out, global const float *in) {\n"
	"	uint i = get_global_id(0);\n"
	"	float x = in[i];\n"
	"	out[i] = (float4)(rint(x), ceil(x), trunc(x), sqrt(x));\n"
	"}\n"
	"kernel void ids(global uint *o) {\n"
	"	uint i = get_global_id(0);\n"
	"	if (i < 3) {\n"
	"		o[8 * i + 0] = get_local_size(0);\n"
	"		o[8 * i + 1] = get_local_size(1);\n"
	"		o[8 * i + 2] = get_num_groups(0);\n"
	"		o[8 * i + 3] = get_global_id(1);\n"
	"		o[8 * i + 4] = get_global_id(2);\n"
	"		o[8 * i + 5] = get_local_linear_id();\n"
	"		o[8 * i + 6] = get_global_size(0);\n"
	"		o[8 * i + 7] = get_group_id(0);\n"
	"	}\n"
	"}\n"
	"kernel void priv(global uint *out, uint n) {\n"
	"	uint a[128], i = get_global_id(0);\n"
	"	for (uint j = 0; j < 128; j++)\n"
	"		a[j] = i * 128 + j;\n"
	"	out[i] = a[(i + n) & 127];\n"
	"}\n";

static int failed;

static void check(const char *name, int bad)
{
	printf("[%s] %s\n", name, bad ? "FAIL" : "ok");
	failed += bad != 0;
}

#define CL(x) do { cl_int e_ = (x); if (e_) { printf("%s: %d\n", #x, e_); return 1; } } while (0)

static cl_int run(cl_command_queue q, cl_kernel k, size_t local)
{
	size_t global = N;

	return clEnqueueNDRangeKernel(q, k, 1, NULL, &global, &local, 0, NULL, NULL);
}

int main(void)
{
	static cl_uint u[N], v[N];
	static float x[N], y[N];
	cl_platform_id plat;
	cl_device_id dev;
	cl_context ctx;
	cl_command_queue q;
	cl_program prog;
	cl_kernel kfill, ksaxpy, krev, kcount, klcount, kh2f, kf2h, krnd, kids, kpriv;
	cl_mem bu, bx, by, bc;
	cl_int err;
	char name[128];
	cl_uint c = 0, k = 0x1000, i, bad, want;
	float a = 2.5f;

	CL(clGetPlatformIDs(1, &plat, NULL));
	CL(clGetDeviceIDs(plat, CL_DEVICE_TYPE_ALL, 1, &dev, NULL));
	CL(clGetDeviceInfo(dev, CL_DEVICE_NAME, sizeof(name), name, NULL));
	printf("device: %s\n", name);
	ctx = clCreateContext(NULL, 1, &dev, NULL, NULL, &err);
	CL(err);
	q = clCreateCommandQueueWithProperties(ctx, dev, NULL, &err);
	CL(err);
	prog = clCreateProgramWithSource(ctx, 1, &src, NULL, &err);
	CL(err);
	if (clBuildProgram(prog, 1, &dev, "-cl-std=CL3.0", NULL, NULL)) {
		static char log[16384];

		clGetProgramBuildInfo(prog, dev, CL_PROGRAM_BUILD_LOG, sizeof(log), log, NULL);
		printf("build failed:\n%s\n", log);
		return 1;
	}
	kfill = clCreateKernel(prog, "fill", &err);
	ksaxpy = clCreateKernel(prog, "saxpy", &err);
	krev = clCreateKernel(prog, "reverse", &err);
	kcount = clCreateKernel(prog, "count", &err);
	klcount = clCreateKernel(prog, "lcount", &err);
	kh2f = clCreateKernel(prog, "h2f", &err);
	kf2h = clCreateKernel(prog, "f2h", &err);
	krnd = clCreateKernel(prog, "rnd", &err);
	kids = clCreateKernel(prog, "ids", &err);
	kpriv = clCreateKernel(prog, "priv", &err);
	CL(err);

	for (i = 0; i < N; i++) {
		x[i] = i * 0.5f;
		y[i] = 1.0f + i;
		v[i] = i * 7 + 3;
	}
	bu = clCreateBuffer(ctx, CL_MEM_READ_WRITE, sizeof(u), NULL, &err);
	bx = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, sizeof(x), x, &err);
	by = clCreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, sizeof(y), y, &err);
	bc = clCreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, sizeof(c), &c, &err);
	CL(err);

	CL(clSetKernelArg(kfill, 0, sizeof(bu), &bu));
	CL(clSetKernelArg(kfill, 1, sizeof(k), &k));
	CL(run(q, kfill, 64));
	CL(clEnqueueReadBuffer(q, bu, CL_TRUE, 0, sizeof(u), u, 0, NULL, NULL));
	for (bad = i = 0; i < N; i++)
		bad += u[i] != i + k;
	check("fill", bad);

	CL(clSetKernelArg(ksaxpy, 0, sizeof(by), &by));
	CL(clSetKernelArg(ksaxpy, 1, sizeof(bx), &bx));
	CL(clSetKernelArg(ksaxpy, 2, sizeof(a), &a));
	CL(run(q, ksaxpy, 64));
	CL(clEnqueueReadBuffer(q, by, CL_TRUE, 0, sizeof(y), y, 0, NULL, NULL));
	for (bad = i = 0; i < N; i++)
		bad += y[i] != a * (i * 0.5f) + 1.0f + i;
	check("saxpy", bad);

	CL(clEnqueueWriteBuffer(q, bu, CL_TRUE, 0, sizeof(v), v, 0, NULL, NULL));
	CL(clSetKernelArg(krev, 0, sizeof(bu), &bu));
	CL(run(q, krev, 64));
	CL(clEnqueueReadBuffer(q, bu, CL_TRUE, 0, sizeof(u), u, 0, NULL, NULL));
	for (bad = i = 0; i < N; i++)
		bad += u[i] != v[i / 64 * 64 + 63 - i % 64];
	check("reverse", bad);

	CL(clEnqueueWriteBuffer(q, bu, CL_TRUE, 0, sizeof(v), v, 0, NULL, NULL));
	CL(clSetKernelArg(kcount, 0, sizeof(bc), &bc));
	CL(clSetKernelArg(kcount, 1, sizeof(bu), &bu));
	CL(run(q, kcount, 64));
	CL(clEnqueueReadBuffer(q, bc, CL_TRUE, 0, sizeof(c), &c, 0, NULL, NULL));
	for (want = i = 0; i < N; i++)
		want += v[i] & 1;
	check("count", c != want);

	c = 0;
	CL(clEnqueueWriteBuffer(q, bc, CL_TRUE, 0, sizeof(c), &c, 0, NULL, NULL));
	CL(clSetKernelArg(klcount, 0, sizeof(bc), &bc));
	CL(clSetKernelArg(klcount, 1, sizeof(bu), &bu));
	CL(run(q, klcount, 64));
	CL(clEnqueueReadBuffer(q, bc, CL_TRUE, 0, sizeof(c), &c, 0, NULL, NULL));
	check("local count", c != want);

	/* every half to float, then floats across the half denormal range back */
	{
		static unsigned short h[NH], h2[NH];
		static float f[NH];
		size_t global = NH, local = 64;
		cl_mem bh = clCreateBuffer(ctx, CL_MEM_READ_WRITE, sizeof(h), NULL, &err);
		cl_mem bf = clCreateBuffer(ctx, CL_MEM_READ_WRITE, sizeof(f), NULL, &err);

		CL(err);
		for (i = 0; i < NH; i++)
			h[i] = i;
		CL(clEnqueueWriteBuffer(q, bh, CL_TRUE, 0, sizeof(h), h, 0, NULL, NULL));
		CL(clSetKernelArg(kh2f, 0, sizeof(bf), &bf));
		CL(clSetKernelArg(kh2f, 1, sizeof(bh), &bh));
		CL(clEnqueueNDRangeKernel(q, kh2f, 1, NULL, &global, &local, 0, NULL, NULL));
		CL(clEnqueueReadBuffer(q, bf, CL_TRUE, 0, sizeof(f), f, 0, NULL, NULL));
		for (bad = i = 0; i < NH; i++) {
			float w = half_to_float(i);

			bad += w == w ? memcmp(&f[i], &w, 4) != 0 : f[i] == f[i];
		}
		check("half to float", bad);

		/* i * 2^-34 steps through every half denormal and the rounding between */
		for (i = 0; i < NH; i++)
			f[i] = (i & 1 ? -1.0f : 1.0f) * (i >> 1) * 0x1p-34f;
		CL(clEnqueueWriteBuffer(q, bf, CL_TRUE, 0, sizeof(f), f, 0, NULL, NULL));
		CL(clSetKernelArg(kf2h, 0, sizeof(bh), &bh));
		CL(clSetKernelArg(kf2h, 1, sizeof(bf), &bf));
		CL(clEnqueueNDRangeKernel(q, kf2h, 1, NULL, &global, &local, 0, NULL, NULL));
		CL(clEnqueueReadBuffer(q, bh, CL_TRUE, 0, sizeof(h2), h2, 0, NULL, NULL));
		for (bad = i = 0; i < NH; i++) {
			/* round to nearest even, in units of 2^-24 */
			unsigned q24 = (i >> 1) >> 10, rem = (i >> 1) & 1023;
			unsigned short w = q24 + (rem > 512 || (rem == 512 && (q24 & 1)));

			w |= i & 1 ? 0x8000 : 0;
			bad += h2[i] != w;
		}
		check("float to half", bad);
	}

	/* rint, ceil, trunc bit-exact; sqrt within 3 ulp */
	{
		static float in[N], out[4 * N];
		static const float edge[] = { 0.0f, -0.0f, 0.5f, -0.5f, 1.5f, 2.5f, -2.5f, -0.4f,
					      0x1p23f, 0x1.fffffep22f, -0x1.000002p23f, INFINITY,
					      -INFINITY, 7.5f, 3.0f, 0x1p-23f };
		cl_mem bi = clCreateBuffer(ctx, CL_MEM_READ_WRITE, sizeof(in), NULL, &err);
		cl_mem bo = clCreateBuffer(ctx, CL_MEM_READ_WRITE, sizeof(out), NULL, &err);
		unsigned bad_r = 0, bad_s = 0;

		CL(err);
		for (i = 0; i < N; i++)
			in[i] = i < 16 ? edge[i] : (int)(i * 2654435761u) * 0x1p-20f;
		CL(clEnqueueWriteBuffer(q, bi, CL_TRUE, 0, sizeof(in), in, 0, NULL, NULL));
		CL(clSetKernelArg(krnd, 0, sizeof(bo), &bo));
		CL(clSetKernelArg(krnd, 1, sizeof(bi), &bi));
		CL(run(q, krnd, 64));
		CL(clEnqueueReadBuffer(q, bo, CL_TRUE, 0, sizeof(out), out, 0, NULL, NULL));
		for (i = 0; i < N; i++) {
			float x = in[i], w[3] = { rintf(x), ceilf(x), truncf(x) }, s = sqrtf(x);

			if (memcmp(&out[4 * i], w, sizeof(w)) && !bad_r++)
				printf("  %a: rint ceil trunc %a %a %a, want %a %a %a\n", x,
				       out[4 * i], out[4 * i + 1], out[4 * i + 2], w[0], w[1], w[2]);
			if (s != s)
				bad_s += out[4 * i + 3] == out[4 * i + 3];
			else if (isinf(s) || s == 0)
				bad_s += out[4 * i + 3] != s;
			else
				bad_s += fabsf(out[4 * i + 3] - s) > 3 * (nextafterf(s, INFINITY) - s);
		}
		check("rint ceil trunc", bad_r);
		check("sqrt", bad_s);
	}

	/* the ids private memory addressing is built from */
	CL(clSetKernelArg(kids, 0, sizeof(bu), &bu));
	CL(run(q, kids, 64));
	CL(clEnqueueReadBuffer(q, bu, CL_TRUE, 0, sizeof(u), u, 0, NULL, NULL));
	for (bad = i = 0; i < 3; i++) {
		cl_uint w[8] = { 64, 1, N / 64, 0, 0, i, N, 0 };

		if (memcmp(&u[8 * i], w, sizeof(w)) && !bad++)
			printf("  thread %u: lsize %u %u groups %u gid %u %u lid %u gsize %u group %u\n",
			       i, u[8 * i], u[8 * i + 1], u[8 * i + 2], u[8 * i + 3], u[8 * i + 4],
			       u[8 * i + 5], u[8 * i + 6], u[8 * i + 7]);
	}
	check("ids", bad);

	/* a private array indexed at run time lives in scratch memory */
	CL(clSetKernelArg(kpriv, 0, sizeof(bu), &bu));
	CL(clSetKernelArg(kpriv, 1, sizeof(k), &k));
	CL(run(q, kpriv, 64));
	CL(clEnqueueReadBuffer(q, bu, CL_TRUE, 0, sizeof(u), u, 0, NULL, NULL));
	for (bad = i = 0; i < N; i++)
		bad += u[i] != i * 128 + ((i + k) & 127);
	check("private array", bad);

	printf("%d failed\n", failed);
	return failed != 0;
}
