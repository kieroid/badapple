# SPDX-License-Identifier: GPL-2.0+ OR MIT
# Copyright (c) 2026 kiki <kieran@lavato.net>
# check      compile the device tree on its own, no kernel tree needed
# deps       clone m1n1, the Hoolock kernel and the docs into build/, and patch them
# patch      apply patches/ to build/m1n1 and build/linux, safe to re-run
# pmgr       regenerate dts/t8020-pmgr.dtsi, needs DT=<DeviceTree im4p>
# m1n1       build the m1n1 chainload payload
# kernel     build Image.gz with Hoolock's 16K config
# initramfs  download Hoolock's test initramfs
# images     one-time: patch iBSS and wrap the device tree and trustcache,
#            needs IPSW=<unpacked ipsw dir>
# kernelcache  pack m1n1, the device tree, the kernel and the initramfs as a
#            fake restore kernelcache that bootx will take
# m1n1cache  pack m1n1 on its own, for poking registers over its proxy
# boot       build that and hand the whole chain to a phone in pwned DFU
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

IBOOT       := build/iboot
MACHO       := $(IBOOT)/m1n1-linux.macho
KERNELCACHE := $(IBOOT)/kernelcache.img4
MONITOR     := $(M1N1_SRC)/build/monitor-stub.macho

# Any manifest does once tools/iboot-image4-patch.py has stopped iBSS looking
# inside one. There is no 0x8020 template and there cannot be: from A11 on a
# manifest is tied to the device's own ECID.
IM4M ?= build/hoolock-docs/binaries/im4m/0x8015.im4m

# Where an unpacked IPSW is, for "make images" only. The file names carry a
# build number, so match on the stem.
IPSW     ?=
IBSS_IM4P = $(firstword $(wildcard $(IPSW)/Firmware/dfu/iBSS.d331p*.im4p))
ADT_IM4P  = $(firstword $(wildcard $(IPSW)/Firmware/all_flash/DeviceTree.d331pap*.im4p))

# console=tty0 is the framebuffer m1n1 hands over, which is readable on the
# phone itself. The shell arrives separately over the USB gadget. earlycon
# picks up UART0 through stdout-path, for anyone with a serial cable.
BOOTARGS ?= earlycon console=tty0 ignore_loglevel

.PHONY: check deps patch pmgr m1n1 kernel initramfs images kernelcache \
	m1n1cache boot clean FORCE
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
$(M1N1): FORCE
	$(MAKE) -C $(M1N1_SRC) RELEASE=1 CHAINLOADING=1 USE_CLANG=1 BUILDSTD=1

kernel: $(IMAGE)

# NBD is disabled because nbd_init hangs the boot on this device: with
# initcall_debug the last line on screen is "calling nbd_init+0x0/0x194" and
# nothing follows. Hoolock ships it enabled for pmOS netboot, which we do not
# use.
#
# Same for CONFIG_ARM_APPLE_SOC_CPUFREQ, which hangs the boot one initcall
# after Bluetooth: requests are accepted and never completed, because the
# coprocessor that arbitrates DVFS is not running. See NOTES.md.
#
# Both have to be reapplied after the config copy, otherwise they are silently
# undone on the next build. Passing KCONFIG_EXTRA does not carry them either,
# which is worth knowing before blaming whatever driver you just added.
#
# KCONFIG_EXTRA takes more scripts/config arguments for one-off debug builds,
# e.g. KCONFIG_EXTRA="--enable CONFIG_SOFTLOCKUP_DETECTOR".
#
# FORCE because make cannot see the kernel sources. Without it an edit under
# build/linux never reaches the kernelcache once Image.gz exists; kbuild itself
# keeps the rerun incremental.
$(IMAGE): FORCE
	cp $(CONFIG_16K) $(KDIR)/.config
	cd $(KDIR) && ./scripts/config --disable CONFIG_BLK_DEV_NBD \
	    --disable CONFIG_ARM_APPLE_SOC_CPUFREQ $(KCONFIG_EXTRA)
	$(MAKE) -C $(KDIR) $(KBUILD) olddefconfig
	$(MAKE) -C $(KDIR) $(KBUILD) -j$$(nproc) Image.gz

initramfs: $(INITRAMFS)

$(INITRAMFS):
	mkdir -p build
	curl -fL -o build/initramfs.gz.zip \
	    'https://nightly.link/HoolockLinux/HoolockRD/workflows/build/master/initramfs.gz.zip'
	cd build && unzip -o initramfs.gz.zip && rm initramfs.gz.zip

# One time, from an unpacked IPSW. iBSS is patched so it will load a payload
# Apple never signed; the trustcache is 24 bytes of header with no entries,
# which is all iBoot wants here.
images: $(IBOOT)/iBSS.patched.raw $(IBOOT)/devicetree.img4 $(IBOOT)/trustcache.img4

$(IBOOT):
	mkdir -p $@

$(IBOOT)/iBSS.raw: | $(IBOOT)
	@test -n "$(IBSS_IM4P)" || { echo "set IPSW=<unpacked ipsw dir>"; exit 1; }
	ipsw img4 im4p extract -o $@ $(IBSS_IM4P)

$(IBOOT)/DeviceTree.bin: | $(IBOOT)
	@test -n "$(ADT_IM4P)" || { echo "set IPSW=<unpacked ipsw dir>"; exit 1; }
	ipsw img4 im4p extract -o $@ $(ADT_IM4P)

$(IBOOT)/iBSS.patched.raw: $(IBOOT)/iBSS.raw tools/iboot-image4-patch.py
	python3 tools/iboot-image4-patch.py $< $@

$(IBOOT)/empty_trustcache.bin: | $(IBOOT)
	printf '\x01\x00\x00\x00\x6e\x17\x68\x17\x88\x2d\x44\x40\x83\xb6\xf3\x22\x06\xbc\x4f\x17\x00\x00\x00\x00' > $@

$(IBOOT)/devicetree.img4: $(IBOOT)/DeviceTree.bin
	ipsw img4 create --input $< --type rdtr --im4m $(IM4M) --output $@

$(IBOOT)/trustcache.img4: $(IBOOT)/empty_trustcache.bin
	ipsw img4 create --input $< --type rtsc --im4m $(IM4M) --output $@

kernelcache: $(KERNELCACHE)

# m1n1 on its own, no Linux, for poking registers over its proxy.
m1n1cache: $(IBOOT)/kernelcache-m1n1.img4

$(IBOOT)/kernelcache-m1n1.img4: $(M1N1) FORCE | $(IBOOT)
	ipsw img4 create --input $(M1N1) --type rkrn --compress lzss \
	    --extra $(MONITOR) --im4m $(IM4M) --output $@

# The payload has to be a Mach-O and has to be lzss compressed, which --extra
# requires anyway for the monitor stub.
#
# FORCE because BOOTARGS is not a file: without it an override on the command
# line, say to add initcall_debug for one boot, is silently ignored whenever
# the kernel itself has not changed. Packing costs a couple of seconds.
$(KERNELCACHE): $(M1N1) $(DTB) $(IMAGE) $(INITRAMFS) FORCE | $(IBOOT)
	{ cat $(M1N1); \
	  echo 'chosen.bootargs=$(BOOTARGS)'; \
	  cat $(DTB) $(IMAGE) $(INITRAMFS); } > $(MACHO)
	ipsw img4 create --input $(MACHO) --type rkrn --compress lzss \
	    --extra $(MONITOR) --im4m $(IM4M) --output $@
	@echo "built $@ ($$(stat -c%s $@) bytes)"

# Phone in DFU on the jig, exploit fires by itself, move the cable here.
# iboot-boot.sh checks for the one-time images itself and says what is missing,
# so this does not depend on them and never goes looking for an IPSW.
boot: $(KERNELCACHE)
	./tools/iboot-boot.sh $(KERNELCACHE)

clean:
	rm -f $(DTB) $(PP) $(MACHO) $(KERNELCACHE)
