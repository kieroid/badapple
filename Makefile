# SPDX-License-Identifier: GPL-2.0+ OR MIT
# Copyright (c) 2026 kiki <kieran@lavato.net>
# check      compile the device tree on its own, no kernel tree needed
# deps       clone m1n1, the Hoolock kernel and the docs into build/, and patch them
# patch      apply patches/ to build/m1n1 and build/linux, safe to re-run
# pmgr       regenerate dts/t8020-pmgr.dtsi, needs DT=<DeviceTree im4p>
# m1n1       build the m1n1 chainload payload
# kernel     build Image.gz with Hoolock's 16K config
# initramfs  download Hoolock's test initramfs
#
# Packing those into a kernelcache and booting it is README.md's "Run", with
# tools/iboot-boot.sh driving the phone.
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

# bootx wants a Mach-O, so this is the one m1n1 build that can be packed as a
# kernelcache. m1n1.bin is raw and gets rejected without a word.
M1N1      ?= $(M1N1_SRC)/build/m1n1-idevice.macho
IMAGE     ?= $(KDIR)/arch/arm64/boot/Image.gz
INITRAMFS ?= build/initramfs.gz

KBUILD := LLVM=1 ARCH=arm64

.PHONY: check deps patch pmgr m1n1 kernel initramfs clean FORCE
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
	./tools/gen-pmgr.sh $(DT)

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
# build/linux never reaches the kernelcache once Image.gz exists; kbuild itself
# keeps the rerun incremental.
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

clean:
	rm -f $(DTB) $(PP)
