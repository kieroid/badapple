// SPDX-License-Identifier: GPL-2.0+ OR MIT
// Copyright (c) 2026 kiki <kieran@lavato.net>
//
// Runs the Mesa-compiled kernels from tools/gfx-tests.cl on the GPU through
// the agx DRM driver (linux-0020, Asahi uAPI) and checks what they wrote. The
// same jobs as "tools/gfx-proxy.py clc", built in a buffer bound at the USC
// base. "make agx-test" builds it static with the kernel's nolibc and puts it
// in the initramfs.

#include <drm/asahi_drm.h>

#include "tests.c"

#define USC_BASE	0x1100000000ULL
#define HEAP_SIZE	0x400000

static int fd;
static uint8_t *heap;
static uint64_t heap_gva, heap_size, heap_off;
static uint32_t queue_id, syncobj;
static int failed;

/* A VM with a heap bound at the USC base and a queue on it. */
static int setup(const struct drm_asahi_params_global *p)
{
	struct drm_asahi_vm_create vm = {
		.kernel_start = p->vm_start, .kernel_end = p->vm_start + p->vm_kernel_min_size,
	};
	struct drm_asahi_gem_create gem = { .size = HEAP_SIZE };
	struct drm_asahi_gem_mmap_offset mo = { 0 };
	struct drm_asahi_gem_bind_op op = {
		.flags = DRM_ASAHI_BIND_READ | DRM_ASAHI_BIND_WRITE,
		.range = HEAP_SIZE, .addr = USC_BASE,
	};
	struct drm_asahi_vm_bind bind = { .num_binds = 1, .stride = sizeof(op) };
	struct drm_asahi_queue_create q = { .usc_exec_base = USC_BASE };

	if (ioctl(fd, DRM_IOCTL_ASAHI_VM_CREATE, &vm) ||
	    ioctl(fd, DRM_IOCTL_ASAHI_GEM_CREATE, &gem))
		return -1;
	mo.handle = op.handle = gem.handle;
	bind.vm_id = q.vm_id = vm.vm_id;
	bind.userptr = (uintptr_t)&op;
	if (ioctl(fd, DRM_IOCTL_ASAHI_GEM_MMAP_OFFSET, &mo) ||
	    ioctl(fd, DRM_IOCTL_ASAHI_VM_BIND, &bind) ||
	    ioctl(fd, DRM_IOCTL_ASAHI_QUEUE_CREATE, &q))
		return -1;
	heap = mmap(NULL, HEAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mo.offset);
	if (heap == MAP_FAILED)
		return -1;
	heap_gva = USC_BASE;
	heap_size = HEAP_SIZE;
	heap_off = 0;
	queue_id = q.queue_id;
	printf("vm %u queue %u\n", vm.vm_id, q.queue_id);
	return 0;
}

/* One compute command, signalling syncobj, then wait on it. */
static int submit(uint64_t stream)
{
	struct {
		struct drm_asahi_cmd_header h;
		struct drm_asahi_cmd_compute c;
	} cmd = {
		.h = { .cmd_type = DRM_ASAHI_CMD_COMPUTE, .size = sizeof(cmd.c),
		       .vdm_barrier = DRM_ASAHI_BARRIER_NONE, .cdm_barrier = 0 },
		.c = { .cdm_ctrl_stream_base = stream, .cdm_ctrl_stream_end = stream + 0x40 },
	};
	struct drm_asahi_sync out = { .sync_type = DRM_ASAHI_SYNC_SYNCOBJ, .handle = syncobj };
	struct drm_asahi_submit sub = {
		.syncs = (uintptr_t)&out, .cmdbuf = (uintptr_t)&cmd, .queue_id = queue_id,
		.out_sync_count = 1, .cmdbuf_size = sizeof(cmd),
	};
	struct drm_syncobj_wait w = {
		.handles = (uintptr_t)&syncobj, .count_handles = 1, .timeout_nsec = INT64_MAX,
	};

	if (ioctl(fd, DRM_IOCTL_ASAHI_SUBMIT, &sub))
		return -1;
	return ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &w);
}

static uint32_t alloc(uint32_t n, uint32_t align)
{
	uint32_t o = (heap_off + align - 1) & ~(align - 1);

	if (o + n > heap_size) {
		printf("heap full\n");
		exit(1);
	}
	heap_off = o + n;
	return o;
}

static uint64_t gva(uint32_t o)
{
	return heap_gva + o;
}

static void put32(uint32_t o, uint32_t v)
{
	memcpy(heap + o, &v, 4);
}

static void put64(uint32_t o, uint64_t v)
{
	memcpy(heap + o, &v, 8);
}

static uint32_t get32(uint32_t o)
{
	uint32_t v;

	memcpy(&v, heap + o, 4);
	return v;
}

/* struct agx_precompiled_kernel_info, then the binary */
struct info {
	uint32_t pre, main, main_size, size, ro;
	uint16_t ro_base, ro16, gprs, pgprs, push, wg[3], local, iblk;
	uint8_t txf;
};

/* Registers in blocks of 8 (16-bit counts, so 16 halves a block), 16 is 0. */
static uint32_t regs(uint32_t halves)
{
	uint32_t n = (halves + 15) / 16;

	return (n ? n : 1) % 16;
}

/*
 * Build the job, run it and return 0 once it is done. Arguments go in
 * uniforms from u0; Mesa counts uniforms in 16-bit units, the uniform load
 * in 32-bit registers. tg overrides the threadgroup memory size in bytes.
 */
static int run(const char *name, const uint32_t *blob, const void *args, uint32_t args_len,
	       uint32_t grid, uint32_t tg)
{
	struct info in;
	const uint8_t *binary = (const uint8_t *)blob + 44;
	uint32_t code, state, ubuf, stream, o, n, hdr, low, word_lo;
	uint64_t word;

	memcpy(&in, blob, 4 * 5);
	memcpy(&in.ro_base, (const uint8_t *)blob + 20, 2 * 10);
	in.txf = ((const uint8_t *)blob)[40];
	if (in.main % 64 || in.ro_base % 2 || in.iblk || in.txf) {
		printf("[%s] unsupported kernel\n", name);
		return -1;
	}

	code = alloc(in.size, 0x4000);
	memcpy(heap + code, binary, in.size);
	ubuf = alloc(args_len + 4, 0x100);
	memcpy(heap + ubuf, args, args_len);
	state = o = alloc(0x100, 0x40);

	/* uniform loads: 3 | first << 9 | count << 21 | VA << 24 */
	n = (args_len + 3) / 4;
	put64(o, gva(ubuf) << 24 | (uint64_t)n << 21 | 3);
	o += 8;
	n = (in.ro16 + 1) / 2;
	if (n) {
		put64(o, (gva(code) + in.ro) << 24 | (uint64_t)n << 21 | (in.ro_base / 2) << 9 | 3);
		o += 8;
	}
	/* shader word: code >> 6 at 38, 9 at 31:28, threadgroup memory 23:16 and 8 */
	if (!tg && in.local)
		tg = in.local + 16;	/* the driver adds 16 bytes */
	word = ((gva(code) + in.main) & 0xffffffff) >> 6 << 38 | 0x90000000 | regs(in.gprs) << 4 | 0xf;
	if (tg)
		word |= (uint64_t)(((tg + 255) / 256) % 256) << 16 | 1 << 8;
	put64(o, word);
	o += 8;
	if (in.pgprs) {
		/* preshader: 0x0038, then 0xc040 | code offset << 16, 48 bits */
		word_lo = 0xc040 | (((uint32_t)(gva(code) + in.pre) & 0xfffe) << 16);
		heap[o] = 0x38;
		heap[o + 1] = 0;
		put32(o + 2, word_lo);
		put32(o + 6, (uint32_t)(gva(code) + in.pre) >> 16);
		o += 8;
	} else {
		heap[o] = 0x08;
		heap[o + 1] = 0x01;
		o += 2;
	}

	/* header: bit 0, uniform blocks of 32 registers; low bits: registers, length */
	hdr = (((in.push + 63) / 64 ? (in.push + 63) / 64 : 1) % 8) << 1 | 1;
	low = regs(in.gprs > in.pgprs ? in.gprs : in.pgprs) << 2 | (((o - state) >> 6) & 3);
	stream = alloc(0x40, 0x4000);	/* like the proxy and the driver */
	put32(stream, hdr);
	put32(stream + 4, ((uint32_t)gva(state) & 0xffffffc0) | low);
	put32(stream + 8, grid);
	put32(stream + 12, 1);
	put32(stream + 16, 1);
	put32(stream + 20, in.wg[0]);
	put32(stream + 24, in.wg[1]);
	put32(stream + 28, in.wg[2]);
	put32(stream + 32, 0xc0000160);
	put32(stream + 36, 0x80000000);

	if (submit(gva(stream))) {
		printf("[%s] run failed: %d\n", name, errno);
		failed++;
		return -1;
	}
	return 0;
}

static uint32_t buf(uint32_t n, uint32_t fill, int ramp)
{
	uint32_t o = alloc(n * 4, 0x4000), i;

	for (i = 0; i < n; i++)
		put32(o + 4 * i, ramp ? fill + i : fill);
	return o;
}

/* want(i) gives the expected word i */
static void expect(const char *name, uint32_t o, uint32_t n, uint32_t (*want)(uint32_t))
{
	uint32_t i, bad = 0, first = 0;

	for (i = 0; i < n; i++) {
		if (get32(o + 4 * i) != want(i)) {
			if (!bad)
				first = i;
			bad++;
		}
	}
	if (bad) {
		printf("[%s] %u wrong, first %u: %x want %x\n", name, bad, first,
		       get32(o + 4 * first), want(first));
		failed++;
	} else {
		printf("[%s] ok\n", name);
	}
}

struct args1 {
	uint64_t a;
} __attribute__((packed));
struct args2 {
	uint64_t a;
	uint32_t b;
} __attribute__((packed));
struct args3 {
	uint64_t a, b;
} __attribute__((packed));

static uint32_t w_fill(uint32_t i) { return 0x1000 + i; }
static uint32_t w_fill2(uint32_t i) { return 0x2000 + i; }
static uint32_t w_loop(uint32_t i)
{
	uint32_t s = 0, j;

	for (j = 0; j < i % 5; j++)
		s += 3 * j + (j & 1 ? 7 : 1);
	return s;
}
static uint32_t w_ids(uint32_t i) { return i % 64 | i / 64 << 16; }
static uint32_t w_id(uint32_t i) { return i; }
static uint32_t w_x3(uint32_t i) { return 3 * i; }
static uint32_t w_swap(uint32_t i) { return i / 64 * 64 + 63 - i % 64; }
static uint32_t w_plus7(uint32_t i) { return i + 7; }
static uint32_t w_plus5(uint32_t i) { return i + 5; }
static uint32_t w_shared(uint32_t g) { return 64 * g * 64 + 64 * 65 / 2; }
static uint32_t w_atomic(uint32_t i) { return 64 * 65 / 2; }
static uint32_t w_simd(uint32_t i) { return 256 * 257 / 2; }
static uint32_t w_256(uint32_t i) { return 256; }

/* one output buffer of 256 words */
static void simple(const char *name, const uint32_t *blob, uint32_t (*want)(uint32_t), uint32_t tg)
{
	uint32_t o = buf(256, 0xdeadbeef, 0);
	struct args1 a = { gva(o) };

	if (!run(name, blob, &a, sizeof(a), 256, tg))
		expect(name, o, 256, want);
}

static void t_float(void)
{
	uint32_t o = buf(64, 0xdeadbeef, 0), i, bad = 0;
	float scale = 0.25f;
	struct args2 a = { gva(o) };

	memcpy(&a.b, &scale, 4);
	if (run("t_float", t_float_0_g13g, &a, sizeof(a), 64, 0))
		return;
	for (i = 0; i < 64; i++) {
		float x = i * 0.25f, want = x * x - 1.0f / (x + 1.0f), got, d, m;

		memcpy(&got, heap + o + 4 * i, 4);
		d = got > want ? got - want : want - got;
		m = want > 1 || want < -1 ? (want > 0 ? want : -want) : 1;
		if (d > 1e-5f * m)
			bad++;
	}
	if (bad) {
		printf("[t_float] %u wrong\n", bad);
		failed++;
	} else {
		printf("[t_float] ok\n");
	}
}

int main(void)
{
	struct drm_asahi_params_global p = { 0 };
	struct drm_asahi_get_params gp = { .pointer = (uintptr_t)&p, .size = sizeof(p) };
	struct drm_syncobj_create sc = { 0 };
	uint32_t o, c;

	fd = open("/dev/dri/renderD128", O_RDWR);
	if (fd < 0 || ioctl(fd, DRM_IOCTL_ASAHI_GET_PARAMS, &gp) ||
	    ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &sc)) {
		printf("no GPU: %d\n", errno);
		return 1;
	}
	syncobj = sc.handle;
	printf("G%u%c chip %x, VA %llx..%llx\n", p.gpu_generation, p.gpu_variant, p.chip_id,
	       (unsigned long long)p.vm_start, (unsigned long long)p.vm_end);
	if (setup(&p)) {
		printf("setup failed: %d\n", errno);
		return 1;
	}

	{
		struct args2 a = { gva(o = buf(256, 0xdeadbeef, 0)), 0x1000 };

		if (!run("t_fill", t_fill_0_g13g, &a, sizeof(a), 256, 0))
			expect("t_fill", o, 256, w_fill);
	}
	t_float();
	{
		struct args2 a = { gva(o = buf(64, 0xdeadbeef, 0)), 5 };

		if (!run("t_loop", t_loop_0_g13g, &a, sizeof(a), 64, 0))
			expect("t_loop", o, 64, w_loop);
	}
	simple("k_ids", k_ids_0_g13g, w_ids, 0);
	simple("k_barrier", k_barrier_0_g13g, w_id, 0);
	simple("k_tgmem", k_tgmem_0_g13g, w_x3, 0);
	simple("k_tgoff", k_tgoff_0_g13g, w_x3, 0);
	simple("k_tgmem@32k", k_tgmem_0_g13g, w_x3, 0x8000);
	simple("k_tgswap", k_tgswap_0_g13g, w_swap, 0);
	simple("k_fence", k_fence_0_g13g, w_plus7, 0);
	{
		uint32_t src = buf(256, 1, 1);
		struct args3 a = { gva(o = buf(4, 0xdeadbeef, 0)), gva(src) };

		if (!run("t_shared", t_shared_0_g13g, &a, sizeof(a), 256, 0))
			expect("t_shared", o, 4, w_shared);
	}
	{
		struct args1 a = { gva(c = buf(256, 0, 1)) };

		if (!run("k_atomic_noret", k_atomic_noret_0_g13g, &a, sizeof(a), 256, 0))
			expect("k_atomic_noret", c, 256, w_plus5);
	}
	{
		struct args3 a = { gva(c = buf(256, 0, 1)), gva(o = buf(256, 0xdeadbeef, 0)) };

		if (!run("k_atomic_ret", k_atomic_ret_0_g13g, &a, sizeof(a), 256, 0)) {
			expect("k_atomic_ret", o, 256, w_id);
			expect("k_atomic_ret", c, 256, w_plus5);
		}
	}
	{
		struct args1 a = { gva(c = buf(256, 0, 1)) };

		if (!run("k_atomic_seqcst", k_atomic_seqcst_0_g13g, &a, sizeof(a), 256, 0))
			expect("k_atomic_seqcst", c, 256, w_plus5);
	}
	{
		struct args1 a = { gva(c = buf(1, 0, 0)) };

		if (!run("k_simd", k_simd_0_g13g, &a, sizeof(a), 256, 0))
			expect("k_simd", c, 1, w_simd);
	}
	{
		struct args1 a = { gva(c = buf(1, 0, 0)) };

		if (!run("k_atomic_uniform", k_atomic_uniform_0_g13g, &a, sizeof(a), 256, 0))
			expect("k_atomic_uniform", c, 1, w_256);
	}
	{
		struct args1 a = { gva(c = buf(1, 0, 0)) };

		if (!run("t_atomic", t_atomic_0_g13g, &a, sizeof(a), 64, 0))
			expect("t_atomic", c, 1, w_atomic);
	}

	/*
	 * A second VM, same addresses, its own heap: t_fill must land there
	 * and leave the first VM's heap alone.
	 */
	{
		uint8_t *first = heap;
		uint32_t before = get32(0);
		struct args2 a;

		if (setup(&p)) {
			printf("second VM failed: %d\n", errno);
			return 1;
		}
		a.a = gva(o = buf(256, 0xdeadbeef, 0));
		a.b = 0x2000;
		if (!run("t_fill vm2", t_fill_0_g13g, &a, sizeof(a), 256, 0))
			expect("t_fill vm2", o, 256, w_fill2);
		heap = first;
		if (get32(0) != before) {
			printf("[vm isolation] first heap changed\n");
			failed++;
		} else {
			printf("[vm isolation] ok\n");
		}
	}

	printf("%d failed\n", failed);
	return failed != 0;
}
