# SPDX-License-Identifier: GPL-2.0+ OR MIT
# Copyright (c) 2026 kiki <kieran@lavato.net>
# check      compile the device tree on its own, no kernel tree needed
# deps       clone m1n1, the Hoolock kernel and the docs into build/
# pmgr       regenerate dts/t8020-pmgr.dtsi from the IPSW
# m1n1       build the m1n1 chainload payload
# kernel     build Image.gz with Hoolock's 16K config
# initramfs  download Hoolock's test initramfs
# blob       concatenate m1n1, the device tree, the kernel and the initramfs
# pongo      drive the phone from DFU to a PongoOS shell with l41ka
# pc-boot    same, but over PongoOS's USB with the cable already on the PC
# boot-m1n1  send m1n1 alone, leaving it parked in its USB proxy
# proxy-boot load kernel, dtb and initramfs over m1n1's proxy (cable on the PC)
#
# "make check" is the only target that works on a bare checkout. The headers
# under include/ are copies of the kernel ones so it needs no kernel tree.

DTS := dts/t8020-d331p.dts
DTB := build/t8020-d331p.dtb
PP  := build/t8020-d331p.dts.pp

CPPFLAGS := -nostdinc -Iinclude -Idts -undef -D__DTS__ -x assembler-with-cpp

KDIR      ?= build/linux
M1N1_SRC  ?= build/m1n1
CONFIG_16K ?= build/hoolock-docs/config_16k

M1N1      ?= $(M1N1_SRC)/build/m1n1.bin
IMAGE     ?= $(KDIR)/arch/arm64/boot/Image.gz
INITRAMFS ?= build/initramfs.gz
BLOB      := build/m1n1-linux.bin

KBUILD := LLVM=1 ARCH=arm64

# console=tty0 is the framebuffer m1n1 hands over, which is readable on the
# phone itself. The interactive shell arrives separately, over the USB CDC-ACM
# gadget the initramfs brings up, as /dev/ttyACM0 on the host.
# earlycon picks up UART0 through stdout-path.
# maxcpus=1 is here because m1n1 has no CPU start offset for T8020 yet, so the
# secondaries would never come up anyway. Drop it once SMP works.
BOOTARGS ?= earlycon console=tty0 ignore_loglevel maxcpus=1

.PHONY: check deps pmgr m1n1 kernel initramfs blob pongo boot-m1n1 pc-boot \
	proxy-boot clean
check: $(DTB)

$(DTB): $(DTS) dts/t8020.dtsi dts/t8020-pmgr.dtsi
	mkdir -p build
	cpp $(CPPFLAGS) $< -o $(PP)
	dtc -I dts -O dtb -o $@ $(PP)
	@echo "built $@"

deps:
	mkdir -p build
	test -d $(M1N1_SRC) || git clone --depth 1 https://github.com/HoolockLinux/m1n1.git $(M1N1_SRC)
	test -d $(KDIR) || git clone --depth 1 https://github.com/HoolockLinux/linux.git $(KDIR)
	test -d build/hoolock-docs || git clone --depth 1 https://github.com/HoolockLinux/docs.git build/hoolock-docs
	test -d build/venv || python3 -m venv --system-site-packages build/venv
	build/venv/bin/pip install -q construct

pmgr:
	./tools/gen-pmgr.sh

m1n1: $(M1N1)

# USE_CLANG=1 because m1n1 otherwise reaches for aarch64-linux-gnu-gcc, and
# clang cross compiles to aarch64 without a separate toolchain installed.
#
# BUILDSTD=1 builds the Rust core and alloc crates from source instead of
# expecting a prebuilt aarch64-unknown-none-softfloat target, which is what you
# want without rustup. m1n1 already exports RUSTC_BOOTSTRAP=1, so the -Z flag
# this needs works on a stable rustc. It does need the Rust standard library
# sources: on Gentoo that is the rust-src USE flag.
BUILDSTD ?= 1

$(M1N1):
	$(MAKE) -C $(M1N1_SRC) RELEASE=1 CHAINLOADING=1 USE_CLANG=1 BUILDSTD=$(BUILDSTD)

kernel: $(IMAGE)

# NBD is disabled because nbd_init hangs the boot on this device: with
# initcall_debug the last line on screen is "calling nbd_init+0x0/0x194" and
# nothing follows. Hoolock ships it enabled for pmOS netboot, which we do not
# use. This has to be reapplied after the config copy, otherwise it is silently
# undone on the next build.
$(IMAGE):
	cp $(CONFIG_16K) $(KDIR)/.config
	cd $(KDIR) && ./scripts/config --disable CONFIG_BLK_DEV_NBD
	$(MAKE) -C $(KDIR) $(KBUILD) olddefconfig
	$(MAKE) -C $(KDIR) $(KBUILD) -j$$(nproc) Image.gz

initramfs: $(INITRAMFS)

$(INITRAMFS):
	mkdir -p build
	curl -fL -o build/initramfs.gz.zip \
	    'https://nightly.link/HoolockLinux/HoolockRD/workflows/build/master/initramfs.gz.zip'
	cd build && unzip -o initramfs.gz.zip && rm initramfs.gz.zip

blob: $(BLOB)

$(BLOB): $(M1N1) $(DTB) $(IMAGE) $(INITRAMFS)
	{ cat $(M1N1); \
	  echo 'chosen.bootargs=$(BOOTARGS)'; \
	  cat $(DTB) $(IMAGE) $(INITRAMFS); } > $@
	@echo "built $@ ($$(stat -c%s $@) bytes)"

# l41ka is the only thing that gets an A12 to PongoOS: it runs usbliter8 from
# the RP2350 and embeds a PongoOS fork with T8020 support, which upstream
# PongoOS does not have. Its laikadbg.py speaks to the board over USB
# 4141:4c41, so the board has to be plugged into this machine with the phone
# connected through it.

# Do not run the stages as separate laikadbg commands. Opening the board calls
# set_configuration(), which re-enumerates it and drops the downstream link, so
# a one-shot "exploit" fires at nothing, returns 0, and leaves pwned false.
# tools/chain.py holds one session open across all the stages instead.
pongo:
	build/venv/bin/python tools/chain.py pongo

# The same boot, but driven from this PC over PongoOS's own USB gadget rather
# than through the jig:
#
#   make pongo        jig: DFU -> exploit -> PongoOS shell
#   move the Lightning cable from the jig to this PC, once
#   make pc-boot      upload the blob and bootm, over 05ac:4141
#
# Linux then comes up with the cable already here for the whole boot, instead
# of binding its gadget to the RP2350 and having to re-present itself after.
pc-boot: $(BLOB)
	build/venv/bin/python tools/pcboot.py $(BLOB)

# Sends m1n1 on its own, with nothing appended, so it lands in uartproxy_run
# instead of handing off. First half of the proxy workflow below.
boot-m1n1: $(M1N1)
	build/venv/bin/python tools/chain.py boot $(M1N1)

# The fast iteration loop, and the one that gives the gadget a chance:
#
#   make boot-m1n1          through the jig, leaves m1n1 sitting in its proxy
#   move the Lightning cable from the jig to this PC
#   make proxy-boot         loads kernel, dtb and initramfs over USB
#
# Two reasons this matters. m1n1's log arrives as text here rather than having
# to be photographed off the screen, and a failed attempt retries without
# re-running the exploit as long as m1n1 survives.
#
# The third reason is the USB gadget itself. Booting the blob through the jig
# means dwc2 probes while the phone is attached to the RP2350, which is the
# host running the exploit, so the gadget binds to that and the cable has to
# move afterwards. Here the cable is already on the PC before the kernel
# starts, which is the arrangement the A11 port gets for free from checkm8.
#
# This was dropped once on the grounds that the cable move "never worked on
# this host". That was wrong: it had already worked, enumerating as
# 1209:316d. Do not remove it again on that argument.
#
# Run ONE proxy session per m1n1 boot. Observed: three sessions worked, the
# fourth got no reply to its very first request, while the phone stayed
# enumerated and the screen's last line was still "Heap limit: 0x811e0c000
# (128 MiB)". The mechanism is unproven, so treat the one-session rule as an
# empirical workaround rather than an explanation.
#
# Pick the port by USB vendor id rather than guessing ttyACM0, which is the
# l41ka board. m1n1's proxy is 1209:316d and exposes two ACM interfaces; the
# first is the proxy.
M1N1DEV ?= $(shell for d in /sys/class/tty/ttyACM*; do \
	p=$$(readlink -f $$d/device); \
	[ "$$(cat $$p/../idVendor 2>/dev/null)" = "1209" ] && \
	    echo /dev/$$(basename $$d) && break; done)
PROXY := M1N1DEVICE=$(M1N1DEV) build/venv/bin/python $(M1N1_SRC)/proxyclient

proxy-boot: $(IMAGE) $(DTB) $(INITRAMFS)
	$(PROXY)/tools/linux.py $(IMAGE) $(DTB) $(INITRAMFS) -b '$(BOOTARGS)'

clean:
	rm -f $(DTB) $(PP) $(BLOB)
