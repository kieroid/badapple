// SPDX-License-Identifier: GPL-2.0+ OR MIT
// Copyright (c) 2026 kiki <kieran@lavato.net>
//
// Runs the Mesa-compiled kernels from tools/gfx-tests.cl on the GPU through
// the agx DRM driver (linux-0020, Asahi uAPI) and checks what they wrote. The
// jobs are built in a buffer bound at the USC base. "make agx-test" builds it
// static with the kernel's nolibc and puts it in the initramfs.

#include <drm/asahi_drm.h>

#include "tests.c"

#define USC_BASE	0x1100000000ULL
#define HEAP_SIZE	0x400000

static int fd;
static uint8_t *heap;
static uint64_t heap_gva, heap_size, heap_off;
static uint32_t vm_id, queue_id, syncobj;
static int failed;

/* A VM with a heap bound at the USC base and a queue on it. */
/* Mesa puts the kernel range in the top 32 GB of the VM, we at the bottom */
static int mesa_vm;

static int setup(const struct drm_asahi_params_global *p)
{
	struct drm_asahi_vm_create vm = {
		.kernel_start = mesa_vm ? p->vm_end - (32ull << 30) : p->vm_start,
		.kernel_end = mesa_vm ? p->vm_end : p->vm_start + p->vm_kernel_min_size,
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
	vm_id = vm.vm_id;
	queue_id = q.queue_id;
	printf("vm %u queue %u\n", vm.vm_id, q.queue_id);
	return 0;
}

/* A BO of size bytes bound at GPU VA va in the current VM, mapped. */
static uint8_t *bind_at(uint64_t va, uint64_t size)
{
	struct drm_asahi_gem_create gem = { .size = size };
	struct drm_asahi_gem_mmap_offset mo = { 0 };
	struct drm_asahi_gem_bind_op op = {
		.flags = DRM_ASAHI_BIND_READ | DRM_ASAHI_BIND_WRITE, .range = size, .addr = va,
	};
	struct drm_asahi_vm_bind bind = { .num_binds = 1, .stride = sizeof(op), .vm_id = vm_id };
	void *m;

	if (ioctl(fd, DRM_IOCTL_ASAHI_GEM_CREATE, &gem))
		return NULL;
	mo.handle = op.handle = gem.handle;
	bind.userptr = (uintptr_t)&op;
	if (ioctl(fd, DRM_IOCTL_ASAHI_GEM_MMAP_OFFSET, &mo) ||
	    ioctl(fd, DRM_IOCTL_ASAHI_VM_BIND, &bind))
		return NULL;
	m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mo.offset);
	return m == MAP_FAILED ? NULL : m;
}

/* n compute commands on queue q, after syncobj in, signalling out; 0 for none. */
/* Table for RGX 0x14020, passed as the sampler heap; 0 for the kernel's */
static uint64_t table_va;

static int submit_on(uint32_t q, const uint64_t *streams, int n, uint32_t in, uint32_t out)
{
	struct {
		struct drm_asahi_cmd_header h;
		struct drm_asahi_cmd_compute c;
	} cmd[2] = { 0 };
	struct drm_asahi_sync s[2] = {
		{ .sync_type = DRM_ASAHI_SYNC_SYNCOBJ, .handle = in },
		{ .sync_type = DRM_ASAHI_SYNC_SYNCOBJ, .handle = out },
	};
	struct drm_asahi_submit sub = {
		.syncs = (uintptr_t)(in ? s : s + 1), .cmdbuf = (uintptr_t)cmd, .queue_id = q,
		.in_sync_count = !!in, .out_sync_count = !!out, .cmdbuf_size = n * sizeof(cmd[0]),
	};
	int i;

	for (i = 0; i < n; i++) {
		cmd[i].h.cmd_type = DRM_ASAHI_CMD_COMPUTE;
		cmd[i].h.size = sizeof(cmd[i].c);
		cmd[i].h.vdm_barrier = DRM_ASAHI_BARRIER_NONE;
		cmd[i].c.cdm_ctrl_stream_base = streams[i];
		cmd[i].c.cdm_ctrl_stream_end = streams[i] + 0x40;
		cmd[i].c.sampler_heap = table_va;
	}
	return ioctl(fd, DRM_IOCTL_ASAHI_SUBMIT, &sub);
}

static int wait_sync(uint32_t h)
{
	struct drm_syncobj_wait w = {
		.handles = (uintptr_t)&h, .count_handles = 1, .timeout_nsec = INT64_MAX,
	};

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
 * Build the job and return its control stream, 0 if the kernel is not
 * supported. Arguments go in uniforms from u0; Mesa counts uniforms in
 * 16-bit units, the uniform load in 32-bit registers. tg overrides the
 * threadgroup memory size in bytes.
 */
/* Probe hooks: extra state words before the shader word, header bits, and
 * how many argument registers the direct uniform load covers (0 for all).
 */
static uint64_t extra[8];
static int nr_extra;
static uint32_t hdr_or, arg_regs, grid_y = 1;

static uint64_t build(const char *name, const uint32_t *blob, const void *args,
		      uint32_t args_len, uint32_t grid, uint32_t tg)
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
		failed++;
		return 0;
	}

	code = alloc(in.size, 0x4000);
	memcpy(heap + code, binary, in.size);
	ubuf = alloc(args_len + 4, 0x100);
	memcpy(heap + ubuf, args, args_len);
	state = o = alloc(0x100, 0x40);

	/* uniform loads: 3 | first << 9 | count << 21 | VA << 24 */
	n = arg_regs ? arg_regs : (args_len + 3) / 4;
	put64(o, gva(ubuf) << 24 | (uint64_t)n << 21 | 3);
	o += 8;
	for (int i = 0; i < nr_extra; i++, o += 8)
		put64(o, extra[i]);
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
	if (hdr_or)
		hdr = (hdr & ~0xe) | hdr_or;
	low = regs(in.gprs > in.pgprs ? in.gprs : in.pgprs) << 2 | (((o - state) >> 6) & 3);
	stream = alloc(0x40, 0x4000);	/* like the proxy and the driver */
	put32(stream, hdr);
	put32(stream + 4, ((uint32_t)gva(state) & 0xffffffc0) | low);
	put32(stream + 8, grid);
	put32(stream + 12, grid_y);
	put32(stream + 16, 1);
	put32(stream + 20, in.wg[0]);
	put32(stream + 24, in.wg[1]);
	put32(stream + 28, in.wg[2]);
	put32(stream + 32, 0xc0000160);
	put32(stream + 36, 0x80000000);
	return gva(stream);
}

/* Build the job, run it and return 0 once it is done. */
static int run(const char *name, const uint32_t *blob, const void *args, uint32_t args_len,
	       uint32_t grid, uint32_t tg)
{
	uint64_t stream = build(name, blob, args, args_len, grid, tg);

	if (!stream)
		return -1;
	if (submit_on(queue_id, &stream, 1, 0, syncobj) || wait_sync(syncobj)) {
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
static uint32_t w_burst(uint32_t i) { return i + 5 * 32; }
static uint32_t w_fill5(uint32_t i) { return 0x1000 + i + 5; }

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

/*
 * G11 loads samplers and some uniforms indirectly, through an index that
 * AGXMetalA12 numbers buffers from 12 in. Not understood yet: fill t_fill's
 * value through an indirect uniform load of index idx, with a table at RGX
 * 0x14020 whose entries point at different values, and the heap starting
 * with its own offsets (see main). With bit 6, indexes 0 and 1 read heap
 * offsets 0x80 and 0x70 and index 2 faults; without it the GPU faults.
 * TAG=1 makes the entries unmapped, so a fault address names one.
 */
static void p_indirect_uniform(uint32_t idx)
{
	uint32_t table = alloc(16 * 8, 0x100), vals = alloc(16 * 16, 0x100), o, i, v;
	struct args2 a = { gva(o = buf(256, 0xdeadbeef, 0)), 0 };

	for (i = 0; i < 16; i++) {
		put32(vals + 16 * i, (i + 1) << 20);
		put64(table + 8 * i, gva(vals + 16 * i));
		/* unmapped and distinct: a fault address names the entry */
		if (getenv("TAG"))
			put64(table + 8 * i, 0x3000000000ull + ((uint64_t)i << 24));
	}
	/* value register u2, bit 6 as AGXMetalA12 sets for its own bindings */
	nr_extra = 0;
	extra[nr_extra++] = 0x1b | 1 << 6 | 2 << 9 | 1 << 21 | (uint64_t)idx << 27;
	arg_regs = 2;		/* just the output pointer */
	table_va = gva(table);
	printf("[indirect uniform] table %llx values %llx\n", (unsigned long long)gva(table),
	       (unsigned long long)gva(vals));
	if (!run("indirect uniform", t_fill_0_g13g, &a, sizeof(a), 256, 0)) {
		v = get32(o);
		printf("[indirect uniform %u] value %#x: entry %d\n", idx, v,
		       v & 0xfffff ? -1 : (int)(v >> 20) - 1);
	}
	nr_extra = 0;
	arg_regs = 0;
	table_va = 0;
	hdr_or = 0;
}

/*
 * Apple's G11 image write, byte for byte (data r5-r8, coordinates r1-r2,
 * state 0), into a 64 x h RGBA32F linear image through a PBE descriptor laid
 * out as AGXMetalA12 builds it. With h > 1 the grid is 2D in 8x8 groups,
 * like Mesa's, and r2 is the row.
 */
static void p_image_write(uint32_t h, int mesa, int gprs16, int tex2, uint64_t hi)
{
	/* Mesa's registers: data r4-r7, coordinates r0-r1 */
	static const uint8_t mcode[] = {
		0xf2, 0x01, 0x10, 0x04,				/* get_sr r0, sr80 */
		0xf2, 0x05, 0x11, 0x04,				/* get_sr r1, sr81 */
		0x62, 0x11, 0x00, 0x00, 0x80, 0x3f,		/* r4 = 1.0 */
		0x62, 0x15, 0x00, 0x00, 0x00, 0x40,		/* r5 = 2.0 */
		0x62, 0x19, 0x00, 0x00, 0x40, 0x40,		/* r6 = 3.0 */
		0x62, 0x1d, 0x00, 0x00, 0x80, 0x40,		/* r7 = 4.0 */
		0x31, 0x21, 0x00, 0x09, 0x00, 0x90, 0x02, 0x00,	/* image_write */
		0x08, 0x01,					/* stop */
	};
	static const uint8_t code[] = {
		0xf2, 0x05, 0x10, 0x04,				/* get_sr r1, sr80 */
		0xf2, 0x09, 0x11, 0x04,				/* get_sr r2, sr81 */
		0x62, 0x15, 0x00, 0x00, 0x80, 0x3f,		/* r5 = 1.0 */
		0x62, 0x19, 0x00, 0x00, 0x00, 0x40,		/* r6 = 2.0 */
		0x62, 0x1d, 0x00, 0x00, 0x40, 0x40,		/* r7 = 3.0 */
		0x62, 0x21, 0x00, 0x00, 0x80, 0x40,		/* r8 = 4.0 */
		0x31, 0x29, 0x02, 0x09, 0x00, 0x90, 0x02, 0x00,	/* image_write */
		0x08, 0x01,					/* stop */
	};
	static uint32_t blob[11 + 16];
	uint32_t img = alloc(1024 * h, 0x4000), desc = alloc(64, 0x40), i, bad = 0;
	uint8_t *b = (uint8_t *)blob, *mem = heap + img;
	uint64_t img_va = gva(img);
	float px[4];

	/* or the image in its own BO at a high VA, like Mesa's */
	if (hi) {
		mem = bind_at(hi, 0x10000);
		img_va = hi;
		if (!mem) {
			printf("[raw image write] bind at %llx failed\n", (unsigned long long)hi);
			failed++;
			return;
		}
	}
	memset(mem, 0, 1024 * h);
	memset(blob, 0, sizeof(blob));
	/* struct info: main 0, size, gprs, push, workgroup */
	blob[2] = blob[3] = sizeof(code);
	if (mesa) {
		blob[2] = blob[3] = sizeof(mcode);
		memcpy(b + 44, mcode, sizeof(mcode));
	}
	b[24] = gprs16 ? 16 : 32;
	b[28] = 8;
	b[30] = h > 1 ? 8 : 64;
	b[32] = h > 1 ? 8 : 1;
	b[34] = 1;
	if (!mesa)
		memcpy(b + 44, code, sizeof(code));

	/* PBE: 2D, linear, R32G32B32A32 float, RGBA, 64 x h */
	put64(desc, 2 | 0x38 << 5 | 4 << 12 | 1 << 17 | 2 << 19 | 3 << 21 | 63ull << 23 |
		    (uint64_t)(h - 1) << 37);
	put64(desc + 8, img_va >> 4 | (uint64_t)(1024 / 16 - 1) << 44);
	put64(desc + 16, 0);
	/* Mesa's texture descriptor for the same image after it, at state 1 */
	put64(desc + 24, 0x00007e01fb444702ull);
	put64(desc + 32, 0x000fc00000000000ull | (img_va >> 4) << 2);
	put64(desc + 40, 0);
	extra[0] = 0x17 | (tex2 ? 2 : 1) << 20 | (gva(desc) >> 2) << 26;
	nr_extra = 1;
	hdr_or = 1 << 1 | 1 | 1 << 4;	/* one texture state block */
	grid_y = h;
	if (!run(h > 1 ? "raw image write 2D" : "raw image write", blob, NULL, 0, 64, 0)) {
		for (i = 0; i < 64 * h; i++) {
			memcpy(px, mem + 16 * i, 16);
			bad += px[0] != 1 || px[1] != 2 || px[2] != 3 || px[3] != 4;
		}
		memcpy(px, mem + 1024 * (h - 1), 16);
		printf("[raw image write %ux%u%s%s%s%s] %s, %u bad, last row pixel 0: %d %d %d %d\n", 64, h,
		       mesa ? " r0/r4" : "", gprs16 ? " 8 regs" : "", tex2 ? " +ts1" : "",
		       hi ? " high VA" : "",
		       bad ? "FAIL" : "ok", bad, (int)px[0], (int)px[1], (int)px[2], (int)px[3]);
		failed += bad != 0;
	}
	nr_extra = 0;
	hdr_or = 0;
	grid_y = 1;
}

/* Mesa's whole iwrite kernel, with its 0.5 in u0, into a 64x64 image */
static void p_mesa_iwrite(void)
{
	static const uint8_t code[] = {
		0xf2, 0x01, 0x10, 0x04, 0xf2, 0x05, 0x11, 0x04, 0x1e, 0x09, 0x40, 0x26,
		0x64, 0x00, 0x00, 0x00, 0x3e, 0x91, 0x0b, 0x04, 0x24, 0x00, 0x3e, 0x95,
		0x0b, 0x24, 0x24, 0x00, 0x3e, 0x99, 0x0b, 0x44, 0x2c, 0x00, 0x7e, 0x1d,
		0x80, 0x09, 0x80, 0x00, 0x31, 0x21, 0x00, 0x09, 0x00, 0x90, 0x02, 0x00,
		0x08, 0x01,
	};
	static uint32_t blob[11 + 16];
	uint32_t img = alloc(0x10000, 0x4000), desc = alloc(64, 0x40), i, bad = 0;
	uint8_t *b = (uint8_t *)blob;
	float half = 0.5f, px[4];

	memset(heap + img, 0, 0x10000);
	memset(blob, 0, sizeof(blob));
	blob[2] = blob[3] = sizeof(code);
	b[24] = 16;
	b[28] = 2;
	b[30] = 8;
	b[32] = 8;
	b[34] = 1;
	memcpy(b + 44, code, sizeof(code));
	put64(desc, 0x000007e01ff24702ull);
	put64(desc + 8, 0x0003f00000000000ull | gva(img) >> 4);
	put64(desc + 16, 0);
	put64(desc + 24, 0x00007e01fb444702ull);
	put64(desc + 32, 0x000fc00000000000ull | (gva(img) >> 4) << 2);
	put64(desc + 40, 0);
	extra[0] = 0x17 | 2 << 20 | (gva(desc) >> 2) << 26;
	nr_extra = 1;
	hdr_or = 1 << 1 | 1 | 1 << 4;
	grid_y = 64;
	if (!run("mesa iwrite", blob, &half, 4, 64, 0)) {
		for (i = 0; i < 64 * 64; i++) {
			memcpy(px, heap + img + 16 * i, 16);
			bad += px[0] != i % 64 || px[1] != i / 64 || px[2] != (i % 64) * (i / 64) ||
			       px[3] != 0.5f;
		}
		printf("[mesa iwrite] %s, %u bad\n", bad ? "FAIL" : "ok", bad);
		failed += bad != 0;
	}
	nr_extra = 0;
	hdr_or = 0;
	grid_y = 1;
}

int main(int argc, char **argv)
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
	mesa_vm = argc > 1 && !strcmp(argv[1], "probe-mesavm");
	if (setup(&p)) {
		printf("setup failed: %d\n", errno);
		return 1;
	}

	if (argc > 1 && !strcmp(argv[1], "indirect")) {
		/* the heap starts with 64 KB of its own byte offsets | 0xa0000000 */
		uint32_t pat = alloc(0x10000, 0x4000), k;

		for (k = 0; k < 0x10000; k += 4)
			put32(pat + k, 0xa0000000 | k);
		for (k = 0; k < (argc > 2 ? argc - 2 : 1); k++)
			p_indirect_uniform(argc > 2 ? atoi(argv[2 + k]) : 8);
		printf("%d failed\n", failed);
		return failed != 0;
	}
	if (argc > 1 && !strncmp(argv[1], "probe", 5)) {
		p_image_write(64, 1, 1, 1, 0);
		p_mesa_iwrite();
		if (argc > 2)
			p_indirect_uniform(8);
		printf("%d failed\n", failed);
		return failed != 0;
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
	 * Async: 32 adds queued back to back, then waited for once. Then a
	 * t_fill and an add that must follow it, across two queues of the VM
	 * through a syncobj, and within one submit.
	 */
	{
		struct drm_asahi_queue_create q2 = { .vm_id = vm_id, .usc_exec_base = USC_BASE };
		struct drm_syncobj_create s1 = { 0 };
		struct args1 a = { gva(c = buf(256, 0, 1)) };
		struct args2 f = { gva(c), 0x1000 };
		uint64_t add = build("async add", k_atomic_noret_0_g13g, &a, sizeof(a), 256, 0);
		uint64_t fill = build("async fill", t_fill_0_g13g, &f, sizeof(f), 256, 0);
		uint64_t both[2] = { fill, add };
		int i, err = 0;

		if (!add || !fill || ioctl(fd, DRM_IOCTL_ASAHI_QUEUE_CREATE, &q2) ||
		    ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &s1)) {
			printf("[async] setup failed: %d\n", errno);
			return 1;
		}
		for (i = 0; i < 32 && !err; i++)
			err = submit_on(queue_id, &add, 1, 0, i == 31 ? syncobj : 0);
		if (err || wait_sync(syncobj)) {
			printf("[async burst] failed: %d\n", errno);
			failed++;
		} else {
			expect("async burst", c, 256, w_burst);
		}

		if (submit_on(queue_id, &fill, 1, 0, s1.handle) ||
		    submit_on(q2.queue_id, &add, 1, s1.handle, syncobj) || wait_sync(syncobj)) {
			printf("[async deps] failed: %d\n", errno);
			failed++;
		} else {
			expect("async deps", c, 256, w_fill5);
		}

		if (submit_on(queue_id, both, 2, 0, syncobj) || wait_sync(syncobj)) {
			printf("[async multi] failed: %d\n", errno);
			failed++;
		} else {
			expect("async multi", c, 256, w_fill5);
		}
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
