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
#define IW 64

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
	"}\n"
	"#define L(k) uint16 a##k = vload16(0, in + ((i * 7 + k * 16) & 4080));\n"
	"kernel void spill(global uint *out, global const uint *in) {\n"
	"	uint i = get_global_id(0);\n"
	"	L(0) L(1) L(2) L(3) L(4) L(5) L(6) L(7) L(8) L(9)\n"
	"	uint16 s = a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9;\n"
	"	uint16 r = a0 * s ^ a1 * (s + 1) ^ a2 * (s + 2) ^ a3 * (s + 3) ^\n"
	"		   a4 * (s + 4) ^ a5 * (s + 5) ^ a6 * (s + 6) ^ a7 * (s + 7) ^\n"
	"		   a8 * (s + 8) ^ a9 * (s + 9);\n"
	"	uint8 h = r.lo + r.hi;\n"
	"	uint4 q = h.lo + h.hi;\n"
	"	out[i] = q.x + q.y + q.z + q.w;\n"
	"}\n";

/* Built only if the device reports images */
static const char *isrc =
	"kernel void isize(global int *out, read_only image2d_t img) {\n"
	"	out[get_global_id(0)] = get_image_width(img) * 1000 + get_image_height(img);\n"
	"}\n"
	"kernel void ibindw(global int *out, write_only image2d_t img) {\n"
	"	if (out[0] == 12345)\n"
	"		write_imagef(img, (int2)(0, 0), (float4)(1.0f));\n"
	"	out[get_global_id(0)] = 7;\n"
	"}\n"
	"kernel void ibindr(global int *out, read_only image2d_t img) {\n"
	"	int v = 7;\n"
	"	if (out[0] == 12345)\n"
	"		v = read_imagef(img, (int2)(0, 0)).x;\n"
	"	out[get_global_id(0)] = v;\n"
	"}\n"
	"kernel void iwrite(write_only image2d_t img) {\n"
	"	int x = get_global_id(0), y = get_global_id(1);\n"
	"	write_imagef(img, (int2)(x, y), (float4)(x, y, x * y, 0.5f));\n"
	"}\n"
	"kernel void iread(global float4 *out, read_only image2d_t img) {\n"
	"	int x = get_global_id(0), y = get_global_id(1);\n"
	"	out[y * get_global_size(0) + x] = read_imagef(img, (int2)(x, y));\n"
	"}\n"
	"kernel void isample(global float4 *out, read_only image2d_t img, sampler_t s) {\n"
	"	int x = get_global_id(0), y = get_global_id(1);\n"
	"	out[y * get_global_size(0) + x] = read_imagef(img, s, (float2)(x + 0.5f, y + 0.5f));\n"
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

int main(int argc, char **argv)
{
	static cl_uint u[N], v[N];
	static float x[N], y[N];
	cl_platform_id plat;
	cl_device_id dev;
	cl_context ctx;
	cl_command_queue q;
	cl_program prog, iprog;
	cl_bool images = CL_FALSE;
	cl_kernel kfill, ksaxpy, krev, kcount, klcount, kh2f, kf2h, krnd, kids, kpriv, kspill;
	cl_kernel kisz = NULL, kbw = NULL, kbr = NULL, kiw = NULL, kir = NULL, kis = NULL;
	cl_mem bu, bv, bx, by, bc;
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
	kspill = clCreateKernel(prog, "spill", &err);
	CL(err);

	CL(clGetDeviceInfo(dev, CL_DEVICE_IMAGE_SUPPORT, sizeof(images), &images, NULL));
	if (images) {
		iprog = clCreateProgramWithSource(ctx, 1, &isrc, NULL, &err);
		CL(err);
		CL(clBuildProgram(iprog, 1, &dev, "-cl-std=CL3.0", NULL, NULL));
		kisz = clCreateKernel(iprog, "isize", &err);
		kbw = clCreateKernel(iprog, "ibindw", &err);
		kbr = clCreateKernel(iprog, "ibindr", &err);
		kiw = clCreateKernel(iprog, "iwrite", &err);
		kir = clCreateKernel(iprog, "iread", &err);
		kis = clCreateKernel(iprog, "isample", &err);
		CL(err);
	}

	for (i = 0; i < N; i++) {
		x[i] = i * 0.5f;
		y[i] = 1.0f + i;
		v[i] = i * 7 + 3;
	}
	bu = clCreateBuffer(ctx, CL_MEM_READ_WRITE, sizeof(u), NULL, &err);
	bx = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, sizeof(x), x, &err);
	by = clCreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, sizeof(y), y, &err);
	bc = clCreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, sizeof(c), &c, &err);
	bv = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, sizeof(v), v, &err);
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

	/* more live values than registers, so the compiler spills */
	{
		size_t wg;

		CL(clGetKernelWorkGroupInfo(kspill, dev, CL_KERNEL_WORK_GROUP_SIZE,
					    sizeof(wg), &wg, NULL));
		printf("  spill kernel: %zu threads per group\n", wg);
	}
	CL(clSetKernelArg(kspill, 0, sizeof(bu), &bu));
	CL(clSetKernelArg(kspill, 1, sizeof(bv), &bv));
	CL(run(q, kspill, 32));
	CL(clEnqueueReadBuffer(q, bu, CL_TRUE, 0, sizeof(u), u, 0, NULL, NULL));
	for (bad = i = 0; i < N; i++) {
		cl_uint s[16] = { 0 }, r[16] = { 0 }, t = 0, j, l;

		for (l = 0; l < 10; l++)
			for (j = 0; j < 16; j++)
				s[j] += v[((i * 7 + l * 16) & 4080) + j];
		for (l = 0; l < 10; l++)
			for (j = 0; j < 16; j++)
				r[j] ^= v[((i * 7 + l * 16) & 4080) + j] * (s[j] + l);
		for (j = 0; j < 16; j++)
			t += r[j];
		bad += u[i] != t;
	}
	check("register spills", bad);

	/* 64x64 RGBA float image: write it from a kernel and read it back,
	 * then read it in a kernel with and without a sampler. Pixel (x, y) is
	 * (x, y, x * y, 0.5).
	 */
	if (!images)
		printf("  no images\n");
	else {
		static float img[IW * IW * 4], got[IW * IW * 4];
		cl_image_format fmt = { CL_RGBA, CL_FLOAT };
		cl_image_desc desc = { .image_type = CL_MEM_OBJECT_IMAGE2D,
				       .image_width = IW, .image_height = IW };
		size_t org[3] = { 0 }, reg[3] = { IW, IW, 1 }, gl[2] = { IW, IW }, lo[2] = { 8, 8 };
		cl_mem bi = clCreateImage(ctx, CL_MEM_READ_WRITE, &fmt, &desc, NULL, &err);
		cl_mem bo = clCreateBuffer(ctx, CL_MEM_READ_WRITE, sizeof(got), NULL, &err);
		cl_sampler smp = clCreateSampler(ctx, CL_FALSE, CL_ADDRESS_CLAMP_TO_EDGE,
						 CL_FILTER_NEAREST, &err);
		CL(err);

		for (i = 0; i < IW * IW; i++) {
			img[4 * i] = i % IW;
			img[4 * i + 1] = i / IW;
			img[4 * i + 2] = (i % IW) * (i / IW);
			img[4 * i + 3] = 0.5f;
		}

		/* binds the image but only reads its descriptor from memory */
		CL(clSetKernelArg(kisz, 0, sizeof(bo), &bo));
		CL(clSetKernelArg(kisz, 1, sizeof(bi), &bi));
		CL(clEnqueueNDRangeKernel(q, kisz, 1, NULL, gl, lo, 0, NULL, NULL));
		CL(clEnqueueReadBuffer(q, bo, CL_TRUE, 0, sizeof(cl_int), &c, 0, NULL, NULL));
		check("image size", c != IW * 1000 + IW);

		/* bind the image for writing, then for reading, but never touch it:
		 * tests the texture and sampler state words alone
		 */
		CL(clSetKernelArg(kbw, 0, sizeof(bo), &bo));
		CL(clSetKernelArg(kbw, 1, sizeof(bi), &bi));
		CL(clEnqueueNDRangeKernel(q, kbw, 1, NULL, gl, lo, 0, NULL, NULL));
		CL(clEnqueueReadBuffer(q, bo, CL_TRUE, 0, sizeof(cl_int), &c, 0, NULL, NULL));
		check("image bind for write", c != 7);
		/* "noread" skips everything that loads samplers */
		if (argc > 1 && !strcmp(argv[1], "noread"))
			goto write;
		CL(clSetKernelArg(kbr, 0, sizeof(bo), &bo));
		CL(clSetKernelArg(kbr, 1, sizeof(bi), &bi));
		CL(clEnqueueNDRangeKernel(q, kbr, 1, NULL, gl, lo, 0, NULL, NULL));
		CL(clEnqueueReadBuffer(q, bo, CL_TRUE, 0, sizeof(cl_int), &c, 0, NULL, NULL));
		check("image bind for read", c != 7);
write:

		memset(got, 0, sizeof(got));
		CL(clEnqueueWriteImage(q, bi, CL_TRUE, org, reg, 0, 0, got, 0, NULL, NULL));
		CL(clSetKernelArg(kiw, 0, sizeof(bi), &bi));
		CL(clEnqueueNDRangeKernel(q, kiw, 2, NULL, gl, lo, 0, NULL, NULL));
		CL(clEnqueueReadImage(q, bi, CL_TRUE, org, reg, 0, 0, got, 0, NULL, NULL));
		check("image write", memcmp(img, got, sizeof(img)) != 0);

		/* "noread" stops here, before anything samples */
		if (argc > 1 && !strcmp(argv[1], "noread"))
			goto out;

		CL(clEnqueueWriteImage(q, bi, CL_TRUE, org, reg, 0, 0, img, 0, NULL, NULL));
		memset(got, 0, sizeof(got));
		CL(clSetKernelArg(kir, 0, sizeof(bo), &bo));
		CL(clSetKernelArg(kir, 1, sizeof(bi), &bi));
		CL(clEnqueueNDRangeKernel(q, kir, 2, NULL, gl, lo, 0, NULL, NULL));
		CL(clEnqueueReadBuffer(q, bo, CL_TRUE, 0, sizeof(got), got, 0, NULL, NULL));
		check("image read", memcmp(img, got, sizeof(img)) != 0);

		memset(got, 0, sizeof(got));
		CL(clSetKernelArg(kis, 0, sizeof(bo), &bo));
		CL(clSetKernelArg(kis, 1, sizeof(bi), &bi));
		CL(clSetKernelArg(kis, 2, sizeof(smp), &smp));
		CL(clEnqueueNDRangeKernel(q, kis, 2, NULL, gl, lo, 0, NULL, NULL));
		CL(clEnqueueReadBuffer(q, bo, CL_TRUE, 0, sizeof(got), got, 0, NULL, NULL));
		check("image sample", memcmp(img, got, sizeof(img)) != 0);
		if (memcmp(img, got, sizeof(img)))
			printf("  pixel 1,1: %g %g %g %g\n", got[4 * (IW + 1)], got[4 * (IW + 1) + 1],
			       got[4 * (IW + 1) + 2], got[4 * (IW + 1) + 3]);

	}

out:
	printf("%d failed\n", failed);
	return failed != 0;
}
