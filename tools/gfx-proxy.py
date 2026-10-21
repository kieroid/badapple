#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+ OR MIT
# Copyright (c) 2026 kiki <kieran@lavato.net>
"""Drive the A12 GPU coprocessor from the m1n1 proxy.

usage: M1N1DEVICE=/dev/ttyACM0 build/venv/bin/python tools/gfx-proxy.py [job] [cpjob]

Boot the m1n1 proxy with the GPU firmware loaded (tools/iboot-boot.sh
build/iboot/kernelcache-m1n1.img4). The script boots the firmware, sends the
init block, powers the GPU with a device control message, then optionally
submits a barrier (job) and a compute command (cpjob). The firmware cannot be
restarted, so each run needs a fresh DFU boot. KTrace goes to
build/gfx/ktrace.bin. Offsets name the firmware functions they come from.
"""

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
for i in range(len(mhz)):
    m.put(hwdata_b + 0xa70 + 4 * i, "<I", mhz[i])
    m.put(hwdata_b + 0xab0 + 0x20 * i, "<8I", *[mv[i]] * 8)
    m.put(hwdata_b + 0xcb0 + 0x20 * i, "<8I", *[mv_sram[i]] * 8)
# UAT handoff: +0x9150 per-context TLB generation (u32), +0x9158 per-context
# TTBR (u64, DevCtrl 3 loads it into RGX 0x17030/0x17038), +0x9160/+0x9164
# active context masks.
m.put(C + 0x9150, "<Q", m.va_of(m.alloc(0x100)))
m.put(C + 0x9158, "<Q", m.va_of(m.alloc(0x4000, 0x4000)))
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
struct.pack_into("<Q", blk, 0x13 * 8, table)
struct.pack_into("<Q", blk, 0x14 * 8, 1)
iface.writemem(init, bytes(blk))
for a, n in ((init, 0x4000), (table, 0x4000), (pa, CHUNK)):
    p.dc_civac(a, n)

chans = [struct.unpack_from("<QQ", m.buf, B + i * 0x10) for i in range(17)]
chans = [(pa + st - VA, pa + rg - VA) for st, rg in chans]
ktrace = []
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
    build_cmd()
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
    submit(0, bar, lambda: m.put(bar, "<IQIIIII", 4, m.va_of(stamp), 0, 1, 0, 0, 0), 1)

if "cpjob" in sys.argv[1:]:
    # Compute command (type 3) whose microsequence only uses firmware ops
    # (trace, store, complete), so no shader or GPU VM. G11 command: +0x10
    # context id, +0x14 event control (m1n1 V12.3 layout), +0x1f0
    # microsequence pointer and size, +0x288 stamp index (slot in bits
    # 14:8), +0x28c event.
    EV = 2
    cmd, evc, useq, scratch = m.alloc(0x300), m.alloc(0x100), m.alloc(0x100), m.alloc(0x100)
    def build():
        ops = struct.pack("<IQQQQ", 0x1a | 0x01ab << 16, 1, 2, 3, 4)        # trace 0x1ab
        ops += struct.pack("<IQI", 0x0e | 2 << 28, m.va_of(scratch), 0xcafe0001)  # store32
        ops += struct.pack("<IQQI", 0x2b, 0, m.va_of(scratch + 8), 1).ljust(0x2c, b"\0")  # complete
        ops += struct.pack("<I", 0x18 | 0x40 << 24)                         # end
        m.buf[useq:useq + len(ops)] = ops
        m.put(cmd, "<IQII", 3, 0, 0, 1)
        m.put(cmd + 0x14, "<Q", m.va_of(evc))
        m.put(cmd + 0x1f0, "<QI", m.va_of(useq), len(ops))
        m.put(cmd + 0x288, "<IB", 0, EV)
    submit(2, cmd, build, EV)
    print(f"[cpjob] scratch {rd32(scratch):#x} stamp {rd32(scratch + 8):#x}")
    # Scheduler state for offline reading; may be stale behind the ASC cache.
    (OUT / "data-cp.bin").write_bytes(iface.readmem(DATA_PA, DATA_SIZE))

try:
    work_until(lambda: False, 5)
finally:
    (OUT / "ktrace.bin").write_bytes(b"".join(ktrace))
    print(f"{len(ktrace)} ktrace events in {OUT / 'ktrace.bin'}")
