// SPDX-License-Identifier: GPL-2.0+ OR MIT
// Copyright (c) 2026 kiki <kieran@lavato.net>
//
// OpenCL through Mesa's Rusticl on the A12 GPU: a few kernels covering
// arguments, floats, threadgroup memory with barriers, and global and
// threadgroup atomics, each
// checked on the CPU.

#define CL_TARGET_OPENCL_VERSION 300
#include <CL/cl.h>
#include <stdio.h>
#include <string.h>

#define N 4096

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
	cl_kernel kfill, ksaxpy, krev, kcount, klcount;
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
	if (clBuildProgram(prog, 1, &dev, "", NULL, NULL)) {
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

	printf("%d failed\n", failed);
	return failed != 0;
}
