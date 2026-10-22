#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+ OR MIT
# Copyright (c) 2026 kiki <kieran@lavato.net>
"""Drive the A12 GPU coprocessor from the m1n1 proxy.

usage: M1N1DEVICE=/dev/ttyACM0 build/venv/bin/python tools/gfx-proxy.py [job] [cpjob | kick | kernel]

Boot the m1n1 proxy with the GPU firmware loaded (tools/iboot-boot.sh
build/iboot/kernelcache-m1n1.img4). The script boots the firmware, sends the
init block, powers the GPU with a device control message, then optionally
submits a barrier (job) and a compute command that only runs firmware ops
(cpjob), kicks the GPU (kick) or runs a shader (kernel). The firmware
cannot be restarted, so each run needs a fresh DFU boot. KTrace goes to
build/gfx/ktrace.bin. Offsets name the firmware functions they come from.
"""

import atexit
import pathlib
import struct
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "build/m1n1/proxyclient"))

from m1n1.setup import *
from m1n1.hw.asc import ASC, R_MBOX_CTRL, R_CPU_CONTROL, R_INBOX1, R_OUTBOX1
from m1n1.fw.asc import StandardASC
from m1n1.fw.asc.base import ASCBaseEndpoint

ASC_BASE = 0x206400000
PS_GFX = 0x23b080388
DATA_PA, DATA_SIZE = 0x8001a4000, 0x8c000   # firmware __DATA, from segment-ranges
OUT = ROOT / "build/gfx"
OUT.mkdir(parents=True, exist_ok=True)

class T8020ASCRegs(RegMap):
    CPU_CONTROL = 0x0044, R_CPU_CONTROL
    INBOX_CTRL  = 0x8108, R_MBOX_CTRL
    OUTBOX_CTRL = 0x810c, R_MBOX_CTRL
    INBOX0      = 0x8800, Register64
    INBOX1      = 0x8808, R_INBOX1
    OUTBOX0     = 0x8830, Register64
    OUTBOX1     = 0x8838, R_OUTBOX1

class GfxEP(ASCBaseEndpoint):
    def handle_msg(self, msg0, msg1):
        # 0x42 only says "check the channels"; drain() does that.
        if msg0 != 0x42 << 48:
            self.log(f"rx {msg0:#018x} type {(msg0 >> 48) & 0xff:#x}")
        return True

class MsgsEP(GfxEP):
    SHORT = "gfx_msgs"

class IntsEP(GfxEP):
    SHORT = "gfx_ints"

class GfxASC(StandardASC):
    ENDPOINTS = {0x20: MsgsEP, 0x21: IntsEP}

    def __init__(self, u, base):
        super().__init__(u, base)
        self.asc = T8020ASCRegs(u, base)

hooks = []

def work_until(cond, timeout):
    end = time.time() + timeout
    while not cond():
        asc.work()
        for h in hooks:
            h()
        if time.time() > end:
            return False
    return True

# iBSS leaves the PMGR rail mask (0xa0000 bits 8+n) set, and in that mode the
# rails never follow the perf state (same as the CPU rails, see m1n1-0012).
# Clearing the bits for rail 2 (GPU SRAM) and rail 4 (GPU) lets the hardware
# copy the GPU DVFS row (0x23b070000 + ps * 0x10) into them on every pstate
# change. Linux boots get this from m1n1-0012.
p.clear32(0x23b0a0000, (1 << 10) | (1 << 12))
p.write32(PS_GFX, p.read32(PS_GFX) | 0xf)
end = time.time() + 1
while (p.read32(PS_GFX) >> 4) & 0xf != 0xf:
    assert time.time() < end, f"ps_gfx stuck at {p.read32(PS_GFX):#x}"

asc = GfxASC(u, ASC_BASE)
asc.mgmt.verbose = 1
ASC.boot(asc)
asc.mgmt.start()
assert work_until(lambda: asc.mgmt.iop_power_state == 0x20, 3), "no IOP power ack"
asc.mgmt.boot_done()
assert work_until(lambda: asc.mgmt.ap_power_state == 0x20, 3), "no AP power ack"
asc.start_ep(0x20)
asc.start_ep(0x21)
work_until(lambda: False, 0.5)

class Bump:
    def __init__(self, va, size):
        self.va, self.off = va, 0
        self.buf = bytearray(size)
    def alloc(self, n, align=0x100):
        self.off = (self.off + align - 1) & ~(align - 1)
        o = self.off
        self.off += n
        assert self.off <= len(self.buf)
        return o
    def va_of(self, o):
        return self.va + o
    def put(self, o, fmt, *v):
        struct.pack_into(fmt, self.buf, o, *v)

# Init block per FUN_00025954. Region layout follows m1n1's G13 InitData
# where the G11 firmware matches it. Everything lives in one chunk that the
# firmware maps at VA through the mem-desc table in init[0x13].
VA = 0xffffffffc0000000
CHUNK = 0x300000
init = u.memalign(0x4000, 0x4000)
table = u.memalign(0x4000, 0x4000)
pa = u.memalign(0x4000, CHUNK)
m = Bump(VA, CHUNK)

B = m.alloc(0x8000, 0x4000)        # RegionB, M1 0x6bc0
C = m.alloc(0x10000, 0x4000)       # RegionC, >= 0xbcec here
# init[5]: firmware mem-desc ring like the kext's AGXArmFirmware +0x1088:
# {state (read idx @0, write idx @0x10), 256 entries of 0x14}.
ST = m.alloc(0x4000, 0x4000)
m.put(ST, "<QQ", m.va_of(m.alloc(0x100)), m.va_of(m.alloc(0x100 * 0x14, 0x4000)))
# 17 channels: 12 run queues, DevCtrl, Event, FWLog (6 rings), KTrace, Stats.
rings = [0x30 * 0x100] * 12 + [0x30 * 0x100, 0x38 * 0x100, 6 * 0xd8 * 0x100,
                               0x70 * 0x200, 0x60 * 0x100]
for i, n in enumerate(rings):
    m.put(B + i * 0x10, "<QQ", m.va_of(m.alloc(0x100)), m.va_of(m.alloc(n, 0x4000)))
hwdata_b = m.alloc(0x4000, 0x4000)
# 0x170/0x178/0x180 TA/3D/CP stats, 0x188 hwdata_a, 0x190 fault info,
# 0x1b0 the firmware's copy of the per-context TLB generations.
for off, n in ((0x170, 0x1000), (0x178, 0x1000), (0x180, 0x1000), (0x188, 0x20000),
               (0x190, 0x80), (0x198, 0xc0), (0x1b0, 0x4000), (0x1b8, 0x1000),
               (0x1c0, 0x300), (0x1c8, 0x1000), (0x220, 0x4000)):
    m.put(B + off, "<Q", m.va_of(m.alloc(n, 0x4000)))
m.put(B + 0x1a0, "<Q", m.va_of(hwdata_b))
m.put(B + 0x1a8, "<Q", m.va_of(hwdata_b))
hwdata_a = struct.unpack_from("<Q", m.buf, B + 0x188)[0] - VA
# hwdata_b IO maps, 0x20 each: phys, fw va (firmware fills it), size, rw.
# 0 Fender and 3 RGX are ADT /arm-io/sgx reg[1] and reg[0].
m.put(hwdata_b + 0x640, "<QQQI", 0x204d00000, 0, 0x20000, 1)
m.put(hwdata_b + 0x6a0, "<QQQI", 0x204000000, 0, 0x20000, 1)
# The firmware maps the others only when the PA is set (FUN_00028ad0) but
# uses them anyway, so point them at scratch RAM.
for off in (0x680, 0x6e0, 0x700, 0x720, 0x740, 0x760, 0x780, 0x7c0, 0x7e0, 0x800,
            0x820, 0x840, 0x880, 0x8a0, 0x8c0, 0x8e0, 0x900):
    m.put(hwdata_b + off, "<QQQI", pa + m.alloc(0x8000, 0x4000), 0, 0x8000, 1)
# Real power mode: the firmware asks Fender for a perf state through
# Fender+0x1000. Tables from ADT sgx/perf-states.
m.put(hwdata_b + 0x958, "<I", 1)
m.put(hwdata_b + 0x960, "<I", 1)
mhz = (0, 316, 456, 612, 740, 900, 1098)
mv = (400, 609, 675, 734, 790, 859, 965)
mv_sram = (400, 728, 737, 787, 850, 925, 1046)
# Sample periods in 24 MHz ticks, 16 ms per ADT gpu-power-sample-period. At
# 0 the power thread's timers fire back to back and starve agx_background.
m.put(hwdata_b + 0x988, "<I", 16000 * 24)
m.put(hwdata_a + 4, "<I", 16000 * 24)
# Highest pstate index; the DVFS loop in FUN_00020b44 walks down from it.
m.put(hwdata_b + 0xa6c, "<I", len(mhz) - 1)
# GPU VA layout as the kext sets it (0x14 << 32 range, 0x11 << 32 bases).
# Power-up loads +0x18 into RGX 0x10000; at 0 every CDM stream fetch faults
# as "PM client access out of range".
m.put(hwdata_b, "<QQQQQ", 0x1400000000, 0xffc00000, 0x1100000000, 0x1100000000, 0x18ffff8000)
# Per-engine unit masks (TA, 3D, CP), copied by FUN_000157b4 at power-up.
# The kext writes (1 << n) - 1; at 0 the scheduler never finds a free unit.
# These are the bits the firmware forces on.
m.put(hwdata_b + 0x16d0, "<III", 3, 7, 1)
for i in range(len(mhz)):
    m.put(hwdata_b + 0xa70 + 4 * i, "<I", mhz[i])
    m.put(hwdata_b + 0xab0 + 0x20 * i, "<8I", *[mv[i]] * 8)
    m.put(hwdata_b + 0xcb0 + 0x20 * i, "<8I", *[mv_sram[i]] * 8)
# GPU VM. The MMU is PowerVR Rogue style with 16K pages (AGXGartG11): a
# page catalogue of u32 entries for VA[39:30], page directories of u64
# entries for VA[29:21] and page tables of u64 entries for VA[20:14].
# Entries per the kext: PC (pd >> 12) << 4 | 1, PD pt | 3 (valid, 16K
# pages), PT page | 1. The VM maps the whole chunk at each GVA. The kext's
# VA ranges are 0x11 (4G), 0x13 (4G), 0x14 (4G), 0x15 (16G) << 32. User VMs
# get MMU contexts 16..63 (kext free list, acc+0x1e84..); lower ones are
# reserved, and a CDM job in context 1 faults as "PM client access out of
# range" (Rogue S7 treats contexts <= 8 as PM ones).
GVAS, SLOT = (0x1100000000, 0x1300000000), 16
pc = m.alloc(0x4000, 0x4000)
for base in GVAS:
    pd = m.alloc(0x4000, 0x4000)
    m.put(pc + (base >> 30) * 4, "<I", (pa + pd) >> 12 << 4 | 1)
    for i in range(CHUNK >> 14):
        if i % 128 == 0:
            pt = m.alloc(0x400, 0x400)
            m.put(pd + ((base >> 21) & 0x1ff) * 8 + i // 128 * 8, "<Q", pa + pt | 3)
        m.put(pt + i % 128 * 8, "<Q", pa + i * 0x4000 | 1)
def gva(o, base=GVAS[0]):
    return base + o
# Per-context state: +0x9150 TLB generations (u32), +0x9158 page catalogue
# PAs, +0x9160.. masks of slots whose PC FUN_00026338 loads into RGX
# 0x17030/0x17038 at every power-up (slot 0 always).
m.put(C + 0x9150, "<Q", m.va_of(m.alloc(0x100)))
ttbr = m.alloc(0x4000, 0x4000)
m.put(C + 0x9158, "<Q", m.va_of(ttbr))
m.put(ttbr + SLOT * 8, "<Q", pa + pc)
m.put(C + 0x9160, "<I", 1 << SLOT)
# Trace categories, copied to DAT_000cbc34 at power-up. AP writes to the
# firmware's own __DATA are not seen, so this is the only way in.
m.put(C, "<I", 0xff)
# The first power-up installs the per-queue-type job handlers
# (FUN_0001f650) only when this is set, then clears it.
m.put(C + 0xb1cc, "<I", 1)

iface.writemem(pa, bytes(m.buf))
iface.writemem(table, struct.pack("<QIIHH", VA, pa >> 12, 0, CHUNK >> 14, 1))
blk = bytearray(0x4000)
struct.pack_into("<Q", blk, 3 * 8, m.va_of(B))
struct.pack_into("<Q", blk, 4 * 8, m.va_of(C))
struct.pack_into("<Q", blk, 5 * 8, m.va_of(ST))
# init[6..0x12]: the MMU format for the firmware's own walks (FUN_00026744),
# as the kext's MMU object sets it: page size, page bits, levels, then per
# level entry size, PA shifts, index shift, entries, table size, valid mask,
# PA mask, index mask.
struct.pack_into("<HBB", blk, 0x30, 0x4000, 14, 3)
for i, lv in enumerate(((4, 4, 12, 30, 0x1000, 1, 0xfffffff0, 0xffc0000000),
                        (8, 10, 10, 21, 0x800, 1, 0xfffffffc00, 0x3fe00000),
                        (8, 12, 12, 14, 0x200, 1, 0xfffffff000, 0x1fc000))):
    struct.pack_into("<BBBBHHQQQ", blk, 0x34 + i * 0x20, *lv[:5], 0x4000, *lv[5:])
struct.pack_into("<Q", blk, 0x13 * 8, table)
struct.pack_into("<Q", blk, 0x14 * 8, 1)
iface.writemem(init, bytes(blk))
for a, n in ((init, 0x4000), (table, 0x4000), (pa, CHUNK)):
    p.dc_civac(a, n)

chans = [struct.unpack_from("<QQ", m.buf, B + i * 0x10) for i in range(17)]
chans = [(pa + st - VA, pa + rg - VA) for st, rg in chans]
ktrace = []
@atexit.register
def save_ktrace():
    (OUT / "ktrace.bin").write_bytes(b"".join(ktrace))
    print(f"{len(ktrace)} ktrace events in {OUT / 'ktrace.bin'}")
def drain():
    # Event 13 (0x38 x 0x100), KTrace 15 (0x38 x 0x200). Channel state:
    # read index @0, write index @0x20.
    for i, size, count in ((13, 0x38, 0x100), (15, 0x38, 0x200)):
        st, rg = chans[i]
        r, w = struct.unpack("<I", iface.readmem(st, 4))[0], struct.unpack("<I", iface.readmem(st + 0x20, 4))[0]
        if r == w or w >= count:
            continue
        data = iface.readmem(rg, size * count)
        for k in range((w - r) % count):
            e = data[((r + k) % count) * size:][:size]
            if i == 15:
                ktrace.append(e)
            else:
                print(f"[event] {e.hex()}")
        iface.writemem(st, struct.pack("<I", w))
        p.dc_civac(st, 0x40)
hooks.append(drain)
asc.send(0x81 << 48 | init, R_INBOX1(EP=0x20))
work_until(lambda: False, 1)

def rd(off, n):
    return iface.readmem(pa + off, n)
def rd32(off):
    return struct.unpack("<I", rd(off, 4))[0]
print(f"init done flag regionC+0xb198 {rd32(C + 0xb198):#x}")

# DevCtrl, channel 12, 0x30-byte messages, doorbell 0x11. Type 0x1b only
# stores msg+4 into DAT_000cc410, so reading it back proves the firmware took
# it. It also brings the GPU up, which sets up the scheduler state jobs need.
st, ring = chans[12]
iface.writemem(ring, struct.pack("<IQ", 0x1b, 0x1122334455667788).ljust(0x30, b"\0"))
iface.writemem(st + 0x20, struct.pack("<I", 1))
p.dc_civac(ring, 0x30)
p.dc_civac(st, 0x40)
asc.send(0x83 << 48 | 0x11, R_INBOX1(EP=0x21))
work_until(lambda: False, 3)
g = struct.unpack("<Q", iface.readmem(DATA_PA + 0xcc410 - 0x44000, 8))[0]
print(f"devctrl read ptr {struct.unpack('<I', iface.readmem(st, 4))[0]}, fw global {g:#x}")

def submit(qtype, cmd, build_cmd, ev):
    # One command queue holding one command, on run channel qtype (0 TA_0,
    # 1 3D_0, 2 CL_0; the doorbell is the channel index). CommandQueueInfo is
    # m1n1's with the context pointer at +0xa4. FUN_000075fc takes the run
    # message, FUN_0001fb60 walks the ring and stores the read index at
    # +0x20, FUN_0001dbec clears has_commands (+0x84) when done.
    ptrs, ring, jl, gbuf = m.alloc(0x100), m.alloc(0x800), m.alloc(0x100), m.alloc(0x4000, 0x4000)
    ctx, info = m.alloc(0x100), m.alloc(0x100)
    lo = cmd    # callers allocate cmd first; the chunk is zero elsewhere
    m.put(ptrs + 0x50, "<I", 0x100)
    m.put(ring, "<Q", m.va_of(cmd))
    # Job list is a TAILQ head {first, last = &first}.
    m.put(jl, "<QQ", 0, m.va_of(jl))
    m.put(ctx, "<BB", 0xff, 0xff)
    m.put(ctx + 5, "<B", 1)
    m.put(ctx + 0x1e, "<B", 0xff)
    m.put(ctx + 0x23, "<B", 2)
    m.put(info, "<QQQQIIIi", m.va_of(ptrs), m.va_of(ring), m.va_of(jl), m.va_of(gbuf), 0, 0, 0, -1)
    m.put(info + 0x30, "<IIQIIIiIiQ", 0, 0, 0xffffffffffff0000, 1, 0, 1, -1, 0x1001, -1, 0)
    m.put(info + 0xa4, "<Q", m.va_of(ctx))
    build_cmd(m.va_of(info))
    assert len(m.buf) == CHUNK, "a slice assignment changed the chunk size"
    iface.writemem(pa + lo, bytes(m.buf[lo:m.off]))
    p.dc_civac(pa + lo, m.off - lo)
    st, rg = chans[qtype]
    iface.writemem(rg, struct.pack("<IQIIIQ", qtype, m.va_of(info), 1, ev, 1, 0).ljust(0x30, b"\0"))
    iface.writemem(st + 0x20, struct.pack("<I", 1))
    p.dc_civac(rg, 0x30)
    p.dc_civac(st, 0x40)
    n0 = len(ktrace)
    asc.send(0x83 << 48 | qtype, R_INBOX1(EP=0x21))
    work_until(lambda: False, 2)
    p.dc_civac(pa + lo, m.off - lo)
    print(f"[job] channel {qtype} read ptr {struct.unpack('<I', iface.readmem(st, 4))[0]}")
    print(f"[job] read/head {struct.unpack('<III', rd(info + 0x20, 12))} event {struct.unpack('<i', rd(info + 0x2c, 4))[0]}"
          f" has_commands {rd32(info + 0x84):#x}")
    print(f"[job] ctx {rd(ctx, 0x28).hex()}")
    print(f"[job] slot0 {rd(gbuf, 0x48).hex()}")
    # Skip the power thread and run-queue scan noise.
    for e in ktrace[n0:]:
        code = struct.unpack_from("<I", e, 0x2c)[0] & 0xffffff
        if code not in (0x60f, 0x618, 0x112, 0x100, 0x101, 0x102, 0x103) and code & 0xf00 != 0x200:
            print("  ", hex(code), [hex(x) for x in struct.unpack_from("<QQQQ", e, 0xc)])

if "job" in sys.argv[1:]:
    # Barrier (type 4) whose wait is already met: no GPU work.
    stamp, bar = m.alloc(0x100), m.alloc(0x100)
    submit(0, bar, lambda info: m.put(bar, "<IQIIIII", 4, m.va_of(stamp), 0, 1, 0, 0, 0), 1)

if "cpjob" in sys.argv[1:]:
    # Compute command (type 3) whose microsequence only uses firmware ops
    # (trace, store, finalize, end), so no shader or GPU VM. G11 command: +0x10
    # context id, +0x14 event control (m1n1 V12.3 layout), +0x1f0
    # microsequence pointer and size, +0x278 completion stamp, +0x290 its
    # index in the event control.
    EV = 2
    cmd, evc, useq, scratch = m.alloc(0x300), m.alloc(0x100), m.alloc(0x100), m.alloc(0x100)
    def build(info):
        ops = struct.pack("<IQQQQ", 0x1a | 0x01ab << 16, 1, 2, 3, 4)        # trace 0x1ab
        ops += struct.pack("<IQI", 0x0e | 2 << 28, m.va_of(scratch), 0xcafe0001)  # store32
        # Finalize compute takes the stamp once its unit state entry is done
        # (+0xc == 3) or the flag at +0x3c is set; end (FUN_0001a810) then
        # writes the stamp, retires the slot and frees the engine unit.
        m.put(scratch + 0x20, "<IIII", 0, 0, 0, 3)
        m.put(scratch + 0x40, "<I", 1)
        fin = bytearray(0x44)
        struct.pack_into("<IQ", fin, 0, 0x2a, m.va_of(scratch + 0x20))
        struct.pack_into("<QI", fin, 0x28, m.va_of(scratch + 8), 1)
        struct.pack_into("<Q", fin, 0x3c, m.va_of(scratch + 0x40))
        ops += fin
        ops += struct.pack("<I", 0x18 | 0x40 << 24)                         # end
        m.buf[useq:useq + len(ops)] = ops
        m.put(cmd, "<IQII", 3, 0, 0, 1)
        m.put(cmd + 0x14, "<Q", m.va_of(evc))
        # Event control: +0 points at the event counter. Completion
        # (FUN_0000f15c) bumps +0xc, and FUN_0000d704 retires it once that
        # equals the counter. Use the stamp the end op writes.
        m.put(evc, "<QII", m.va_of(scratch + 8), 0, 0)
        m.put(cmd + 0x1f0, "<QI", m.va_of(useq), len(ops))
        # Completion stamp {target, ?, value, event}: FUN_0000d704 writes the
        # value to target and raises the event.
        m.put(cmd + 0x278, "<QQII", m.va_of(scratch + 0x10), 0, 0xcafe0002, EV)
    submit(2, cmd, build, EV)
    print(f"[cpjob] scratch {rd32(scratch):#x} stamp {rd32(scratch + 8):#x} done {rd32(scratch + 0x10):#x}")
    # Scheduler state for offline reading; may be stale behind the ASC cache.
    (OUT / "data-cp.bin").write_bytes(iface.readmem(DATA_PA, DATA_SIZE))
    (OUT / "chunk-cp.bin").write_bytes(iface.readmem(pa, m.off))

KERNEL = "kernel" in sys.argv[1:]
if "kick" in sys.argv[1:] or KERNEL:
    # A compute command in MMU context SLOT. With "kick" its CDM control
    # stream only terminates; with "kernel" it launches a shader that stores
    # 0xcafe1234 to memory. Microsequence as the kext builds it
    # (FUN_fffffff0084d0f08): StartCompute, wait for the CDM interrupt,
    # finalize, end. StartCompute writes cmd+0x70/+0x78/+0x80 to RGX
    # 0x14020/0x14000/0x118d0 (+0x78 is the stream, like m1n1's G13
    # ComputeInfo encoder), (slot | context << 8) to 0x10200, and kicks
    # 0x10180. The unit state array is stats_cp.
    EV = 3
    stats_cp = struct.unpack_from("<Q", m.buf, B + 0x180)[0]
    cmd, evc, useq, scratch = m.alloc(0x300), m.alloc(0x100), m.alloc(0x200), m.alloc(0x100)
    stream, args = m.alloc(0x4000, 0x4000), m.alloc(0x4000, 0x4000)
    code, state, target = m.alloc(0x400, 0x4000), m.alloc(0x40, 0x40), m.alloc(0x100)
    def build(info):
        if KERNEL:
            # G11 code: the ISA is close to G13 (applegpu decodes G11 kernels
            # from the iOS CoreImage binary archives, Mach-O cpusubtype 0x72),
            # but stop is 0x0801. 32 threads store to target via r0_r1.
            t = gva(target)
            prog = (
                bytes.fromhex("6201") + struct.pack("<I", t & 0xffffffff) +   # mov_imm r0, lo
                bytes.fromhex("6205") + struct.pack("<I", t >> 32) +          # mov_imm r1, hi
                bytes.fromhex("6209") + struct.pack("<I", 0xcafe1234) +       # mov_imm r2
                bytes.fromhex("4511000500c01200") +                           # device_store r2
                bytes.fromhex("3800") + bytes.fromhex("0801"))                # wait 0, stop
            m.buf[code:code + len(prog)] = prog
            # State-load program (ESLStateLoadEncoderGen1): the shader word
            # (code >> 6 at bit 38, 0x9 at 31:28, register blocks 0x1f), then
            # 0x0108 for "no preshader". Addresses are 32-bit offsets from the
            # 0x11 << 32 base in RGX 0x10000.
            m.put(state, "<QH", (gva(code) & 0xffffffff) >> 6 << 38 | 0x90000000 | 0x1f, 0x0108)
            # CDM: launch (block type 0 in bits 31:30, direct), state block,
            # global size in threads, threadgroup size; barrier; terminate.
            m.put(stream, "<8III", 0, gva(state) & 0xffffffc0, 32, 1, 1, 32, 1, 1,
                  0xc0000160, 0x80000000)
        else:
            m.put(stream, "<I", 0x80000000)                 # CDM stream terminate
        m.put(scratch + 0x20, "<Q", 1)                      # kick gate, > cmd+4
        st = bytearray(0x64)
        struct.pack_into("<IQQQQIIIIII", st, 0, 0x29, m.va_of(cmd + 0x1c), m.va_of(cmd + 0x70),
                         stats_cp, info, SLOT, 0, SLOT, 0, 0, 0)
        struct.pack_into("<QQQQQ", st, 0x3c, m.va_of(cmd + 0x1fc), 0, m.va_of(cmd + 0x2d8), 0,
                         m.va_of(scratch + 0x20))
        fin = bytearray(0x44)
        struct.pack_into("<IQQIQII", fin, 0, 0x2a, stats_cp, info, SLOT, m.va_of(cmd + 0x1fc), 0, 0)
        struct.pack_into("<QIIiQ", fin, 0x28, m.va_of(scratch + 8), 1, 0, -(0x64 + 4),
                         m.va_of(cmd + 0x2d8))
        ops = bytes(st) + struct.pack("<I", 0x41) + bytes(fin) + struct.pack("<I", 0x18 | 0x40 << 24)
        m.buf[useq:useq + len(ops)] = ops
        m.put(cmd, "<IQII", 3, 0, 0, SLOT)
        m.put(cmd + 0x14, "<Q", m.va_of(evc))
        m.put(evc, "<QII", m.va_of(scratch + 8), 0, 0)
        m.put(cmd + 0x70, "<QQQ", gva(args), gva(stream), 0)
        m.put(cmd + 0x1f0, "<QI", m.va_of(useq), len(ops))
        m.put(cmd + 0x278, "<QQII", m.va_of(scratch + 0x10), 0, 0xcafe0003, EV)
    submit(2, cmd, build, EV)
    print(f"[kick] stamp {rd32(scratch + 8):#x} done {rd32(scratch + 0x10):#x} stats_cp[0] {rd(stats_cp - VA, 0x20).hex()}")
    if KERNEL:
        print(f"[kernel] target {rd(target, 0x10).hex()}")
    (OUT / "data-kick.bin").write_bytes(iface.readmem(DATA_PA, DATA_SIZE))
    (OUT / "chunk-kick.bin").write_bytes(iface.readmem(pa, m.off))
    # MMU fault status, decoded like the kext (FUN_fffffff0084cf9c8). RGX
    # reads from the AP hang while the GPU is off, so check Fender first.
    if p.read32(0x204d01000) & 0x1f:
        st = p.read64(0x204017040)
        types = ("page invalid", "AF fault", "read-only", "PM protected", "PM read out of range",
                 "PM write out of range", "PM client access out of range", "PM context not loaded")
        print(f"[kick] mmu fault {st:#x}: valid {st & 1} va {st >> 24 & 0xffffffffc0:#x} "
              f"type {types[st >> 1 & 7]} read {st >> 4 & 1} level {st >> 5 & 3} "
              f"requestor {st >> 8 & 0xff} context {st >> 16 & 0x3f}")
        for off in (0x10000, 0x10200, 0x11870, 0x11878, 0x118d0, 0x14000, 0x14020, 0x14040,
                    0x17018, 0x17028, 0x17030, 0x17038, 0x17048):
            print(f"[kick] rgx {off:#x} = {p.read64(0x204000000 + off):#x}")

work_until(lambda: False, 5)
