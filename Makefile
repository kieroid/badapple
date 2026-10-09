# SPDX-License-Identifier: GPL-2.0+ OR MIT
# Copyright (c) 2026 kiki <kieran@lavato.net>
# check      compile the device tree on its own, no kernel tree needed
# deps       clone m1n1, the Hoolock kernel and the docs into build/, and patch them
# patch      apply patches/ to build/m1n1 and build/linux, safe to re-run
# pmgr       regenerate dts/t8020-pmgr.dtsi from the IPSW
# m1n1       build the m1n1 chainload payload
# kernel     build Image.gz with Hoolock's 16K config
# initramfs  download Hoolock's test initramfs
# blob       concatenate m1n1, the device tree, the kernel and the initramfs
# pongoos    build PongoOS with T8020 support
# pongo      upload PongoOS to a phone the jig has left in pwned DFU
# pc-boot    upload the blob to PongoOS, over its own USB gadget
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
PONGO_SRC ?= build/pongoos
PONGO_RAW ?= $(PONGO_SRC)/build/Pongo.bin
PONGO     ?= build/Pongo-a12.bin

# PongoOS links Mach-O, which the system ld cannot do. ld64.lld can, but it
# wants the arch and the platform spelled out because PongoOS's own Makefile
# passes neither.
LLVM_BINDIR ?= $(shell dirname $$(readlink -f $$(command -v clang)))
PONGO_LD ?= -fuse-ld=$(LLVM_BINDIR)/ld64.lld -Wl,-arch,arm64 \
	    -Wl,-platform_version,ios,12.0,12.0

KBUILD := LLVM=1 ARCH=arm64

# console=tty0 is the framebuffer m1n1 hands over, which is readable on the
# phone itself. The interactive shell arrives separately, over the USB CDC-ACM
# gadget the initramfs brings up, as /dev/ttyACM0 on the host.
# earlycon picks up UART0 through stdout-path.
BOOTARGS ?= earlycon console=tty0 ignore_loglevel

.PHONY: check deps patch pmgr m1n1 kernel initramfs blob pongoos pongo pc-boot \
	clean FORCE
FORCE:

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
	$(MAKE) patch

# Neither tree boots this phone unpatched, so a checkout that skips this builds
# cleanly and then does nothing on the hardware. "deps" runs it, and it is also
# a target of its own because the patches get regenerated as work goes on.
#
# Re-running is safe: a patch that is already applied reverses cleanly, so it
# is reported and skipped. A patch that does neither is a real conflict and
# stops the build rather than being swallowed.
patch:
	@set -e; \
	for p in $(CURDIR)/patches/m1n1-*.patch $(CURDIR)/patches/linux-*.patch; do \
	    case $$p in *linux-*) d=$(KDIR);; *) d=$(M1N1_SRC);; esac; \
	    if git -C $$d apply --reverse --check $$p 2>/dev/null; then \
	        echo "  already applied: $$(basename $$p)"; \
	    else \
	        echo "  applying: $$(basename $$p)"; \
	        git -C $$d apply $$p; \
	    fi; \
	done

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

$(M1N1): FORCE
	$(MAKE) -C $(M1N1_SRC) RELEASE=1 CHAINLOADING=1 USE_CLANG=1 BUILDSTD=$(BUILDSTD)

kernel: $(IMAGE)

# NBD is disabled because nbd_init hangs the boot on this device: with
# initcall_debug the last line on screen is "calling nbd_init+0x0/0x194" and
# nothing follows. Hoolock ships it enabled for pmOS netboot, which we do not
# use.
#
# It has to be reapplied after the config copy, otherwise it is silently
# undone on the next build.
#
# KCONFIG_EXTRA takes more scripts/config arguments for one-off debug builds,
# e.g. KCONFIG_EXTRA="--enable CONFIG_SOFTLOCKUP_DETECTOR".
#
# FORCE because make cannot see the kernel sources. Without it an edit under
# build/linux never reaches the blob once Image.gz exists; kbuild itself keeps
# the rerun incremental.
$(IMAGE): FORCE
	cp $(CONFIG_16K) $(KDIR)/.config
	cd $(KDIR) && ./scripts/config --disable CONFIG_BLK_DEV_NBD $(KCONFIG_EXTRA)
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

# PongoOS with T8020 support, which upstream does not have. MIT, same as
# upstream it forks. Nothing from it is vendored here, see NOTICES.md.
pongoos: $(PONGO)

$(PONGO_RAW):
	mkdir -p build
	test -d $(PONGO_SRC) || git clone --depth 1 -b t8027 \
	    https://github.com/0cyn/PongoOS.git $(PONGO_SRC)
	$(MAKE) -C $(PONGO_SRC) EMBEDDED_CFLAGS="$(PONGO_LD)" build/Pongo.bin

# PongoOS wants the SRAM base and a next stage handed to it, which the bootrom
# does not do. The shim sets those up, see the comment in it.
$(PONGO): $(PONGO_RAW) tools/a12-pongo-shim.S
	clang --target=aarch64-none-elf -c tools/a12-pongo-shim.S -o build/shim.o
	llvm-objcopy -O binary --only-section=.text build/shim.o build/shim.bin
	cat build/shim.bin $(PONGO_RAW) > $@
	@echo "built $@ ($$(stat -c%s $@) bytes)"

# Getting an A12 to PongoOS, with the jig doing nothing but the exploit.
#
# The RP2350 runs usbliter8 alone: plug a phone in DFU into it and the bootrom
# exploit fires by itself, leaving the phone in DFU with a patched request
# handler and PWND in its USB serial. Everything after that is this PC talking
# to the phone directly.
#
#   phone in DFU on the jig, wait a second for the exploit
#   move the Lightning cable from the jig to this PC, once
#   make pongo        upload PongoOS to the pwned bootrom and run it
#   make pc-boot      upload the blob and bootm, over 05ac:4141
#
# An earlier version let the jig firmware drive all of this. That firmware got
# to PongoOS by patching iBoot's auto-boot path and installing a second stage
# along it, so it stopped working the moment the phone had no bootable OS left
# on its NAND. Handing the image straight to the bootrom does not care what is
# on the flash, and keeps the licensing of this repo simple.
pongo: $(PONGO)
	build/venv/bin/python tools/dfuboot.py $(PONGO)

# Linux comes up with the cable already here for the whole boot, instead of
# binding its gadget to the RP2350 and having to re-present itself after.
pc-boot: $(BLOB)
	build/venv/bin/python tools/pcboot.py $(BLOB)

clean:
	rm -f $(DTB) $(PP) $(BLOB) $(PONGO) build/shim.o build/shim.bin
