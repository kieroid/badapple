# SPDX-License-Identifier: GPL-2.0+ OR MIT
# Copyright (c) 2026 kiki <kieran@lavato.net>
# check      compile the device tree on its own, no kernel tree needed
# deps       clone m1n1, the Hoolock kernel and the docs into build/, and patch them
# patch      apply patches/ to build/m1n1, build/linux and, once cloned,
#            build/mesa; safe to re-run
# pmgr       regenerate dts/t8020-pmgr.dtsi, needs DT=<DeviceTree im4p>
# m1n1       build the m1n1 chainload payload
# kernel     build Image.gz with Hoolock's 16K config
# initramfs  download Hoolock's test initramfs
# ticket     optional: fetch an Apple ticket that lists coprocessor firmware,
#            needs the phone in DFU and TICKET_IPSW=<a signed .ipsw>
# images     one-time: patch iBSS and wrap the device tree and trustcache,
#            needs IPSW=<unpacked ipsw dir>
# kernelcache  pack m1n1, the device tree, the kernel and the initramfs as a
#            fake restore kernelcache that bootx will take
# m1n1cache  pack m1n1 on its own, for poking registers over its proxy
# boot       build that and hand the whole chain to a phone in pwned DFU
# rootfs     clone and patch Mesa, cross-build Rusticl and GLES for the phone
#            against an Alpine aarch64 sysroot and pack them with
#            tools/cl-test.c and tools/gl-test.c into a rootfs tarball;
#            tools/rootfs-push.sh copies that to a booted phone
#
# "make check" is the only target that works on a bare checkout. The headers
# under include/ are copies of the kernel ones so it needs no kernel tree.

DTS := dts/t8020-d331p.dts
DTB := build/t8020-d331p.dtb
PP  := build/t8020-d331p.dts.pp

CPPFLAGS := -nostdinc -Iinclude -Idts -undef -D__DTS__ -x assembler-with-cpp

KDIR      ?= build/linux
M1N1_SRC  ?= build/m1n1
CONFIG_16K := build/hoolock-docs/config_16k

# bootx wants a Mach-O, so this is the one m1n1 build that can be packed as a
# kernelcache. m1n1.bin is raw and gets rejected without a word.
M1N1      := $(M1N1_SRC)/build/m1n1-idevice.macho
IMAGE     := $(KDIR)/arch/arm64/boot/Image.gz
INITRAMFS := build/initramfs.gz

KBUILD := LLVM=1 ARCH=arm64

IBOOT       := build/iboot
MACHO       := $(IBOOT)/m1n1-linux.macho
KERNELCACHE := $(IBOOT)/kernelcache.img4
MONITOR     := $(M1N1_SRC)/build/monitor-stub.macho

# Any manifest does once tools/iboot-image4-patch.py has stopped iBSS looking
# inside one. There is no 0x8020 template and there cannot be: from A11 on a
# manifest is tied to the device's own ECID.
IM4M := build/hoolock-docs/binaries/im4m/0x8015.im4m

# Where an unpacked IPSW is, for "make images" only. The file names carry a
# build number, so match on the stem.
IPSW     ?=
IBSS_IM4P = $(firstword $(wildcard $(IPSW)/Firmware/dfu/iBSS.d331p*.im4p))
ADT_IM4P  = $(firstword $(wildcard $(IPSW)/Firmware/all_flash/DeviceTree.d331pap*.im4p))
MTFW_IM4P = $(firstword $(wildcard $(IPSW)/Firmware/D331p_Multitouch.im4p))
GFX_IM4P  = $(firstword $(wildcard $(IPSW)/Firmware/agx/armfw_g11p.im4p))
# The .ipsw file itself, for the wifi and Bluetooth firmware, which sits in the
# root filesystem image. iOS 16.6.1's is not encrypted.
IPSW_FILE ?=

# console=tty0 is the framebuffer m1n1 hands over, which is readable on the
# phone itself. The shell arrives separately over the USB gadget. earlycon
# picks up UART0 through stdout-path, for anyone with a serial cable.
BOOTARGS ?= earlycon console=tty0 ignore_loglevel cma=64M

.PHONY: check deps patch pmgr m1n1 kernel initramfs images ticket kernelcache \
	m1n1cache boot rootfs clean FORCE
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
	for p in $(CURDIR)/patches/m1n1-*.patch $(CURDIR)/patches/linux-*.patch \
	         $(CURDIR)/patches/mesa-*.patch; do \
	    case $$p in \
	    *linux-*) d=$(KDIR);; \
	    *mesa-*) d=$(MESA_SRC); test -d $$d || continue;; \
	    *) d=$(M1N1_SRC);; \
	    esac; \
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
# These have to be reapplied after the config copy, otherwise they are silently
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
	    --enable CONFIG_ARM_APPLE_SOC_CPUFREQ \
	    --enable CONFIG_SENSORS_APPLE_SOC_TEMP \
	    --enable CONFIG_CPU_THERMAL \
	    --enable CONFIG_WIRELESS --enable CONFIG_CFG80211 --enable CONFIG_WLAN \
	    --enable CONFIG_WLAN_VENDOR_BROADCOM \
	    --enable CONFIG_BRCMFMAC --enable CONFIG_BRCMFMAC_PCIE \
	    --enable CONFIG_BT_HCIBCM4377 \
	    --disable CONFIG_FW_LOADER_USER_HELPER \
	    --enable CONFIG_DRM_APPLE_H11P \
	    --enable CONFIG_INPUT_TOUCHSCREEN \
	    --enable CONFIG_TOUCHSCREEN_APPLE_MT_SPI \
	    --enable CONFIG_DRM_AGX $(KCONFIG_EXTRA)
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
images: $(IBOOT)/iBSS.patched.raw $(IBOOT)/devicetree.img4 $(IBOOT)/trustcache.img4 \
	$(IBOOT)/mtfw.cpio $(IBOOT)/bcmfw.cpio $(if $(wildcard $(TICKET)),$(IBOOT)/gfx.img4)

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

# The touch controller's firmware, as the op list apple-mt-spi replays, in a
# cpio that gets added to the initramfs. Apple's, so it never leaves build/.
$(IBOOT)/mtfw.cpio: tools/mtfw.py | $(IBOOT)
	@test -n "$(MTFW_IM4P)" || { echo "set IPSW=<unpacked ipsw dir>"; exit 1; }
	ipsw img4 im4p extract -o $(IBOOT)/mtfw.plist $(MTFW_IM4P)
	rm -rf $(IBOOT)/mtfw-root
	mkdir -p $(IBOOT)/mtfw-root/lib/firmware/apple
	python3 tools/mtfw.py $(IBOOT)/mtfw.plist \
	    $(IBOOT)/mtfw-root/lib/firmware/apple/d331p-mtfw.bin
	cd $(IBOOT)/mtfw-root && find . | cpio -o -H newc > ../mtfw.cpio

# The BCM4377's wifi and Bluetooth firmware, renamed for brcmfmac and
# hci_bcm4377. Apple's too, so it stays in build/ as well.
$(IBOOT)/bcmfw.cpio: tools/bcmfw.py | $(IBOOT)
	@test -n "$(IPSW_FILE)" || { echo "set IPSW_FILE=<.ipsw file>"; exit 1; }
	rm -rf $(IBOOT)/ios-root $(IBOOT)/bcmfw-root
	ipsw extract --files --pattern '.*(C-4377__s-B2/|bluetooth/BCM4377B2).*' \
	    -o $(IBOOT)/ios-root $(IPSW_FILE)
	python3 tools/bcmfw.py $(IBOOT)/ios-root/*/root $(IBOOT)/bcmfw-root
	cd $(IBOOT)/bcmfw-root && find . | cpio -o -H newc > ../bcmfw.cpio

# Optional. Coprocessor firmware, like the GPU's, is loaded by iBoot, and iBoot
# only takes it with a manifest that lists it. None of the borrowed ones do, so
# this asks Apple's TSS server for a ticket for your phone, once, with the phone
# in DFU. TICKET_IPSW is any .ipsw Apple still signs for it, and the ticket keeps
# working after that. ipsw's own TSS request gets refused for this phone,
# idevicerestore's does not; -t saves the ticket and exits.
TICKET := $(IBOOT)/apticket.im4m

ticket: | $(IBOOT)
	@test -n "$(TICKET_IPSW)" || { echo "set TICKET_IPSW=<a signed .ipsw>"; exit 1; }
	rm -rf $(IBOOT)/tss
	idevicerestore -t -C $(IBOOT)/tss $(TICKET_IPSW)
	python3 -c 'import gzip, plistlib, sys; \
	    sys.stdout.buffer.write(plistlib.loads(gzip.decompress(open(sys.argv[1], "rb").read()))["ApImg4Ticket"])' \
	    $(IBOOT)/tss/shsh/*.shsh > $(TICKET)

# The GPU coprocessor's firmware, sent with the firmware command.
$(IBOOT)/gfx.img4: $(TICKET) | $(IBOOT)
	@test -n "$(GFX_IM4P)" || { echo "set IPSW=<unpacked ipsw dir>"; exit 1; }
	ipsw img4 create --im4p $(GFX_IM4P) --im4m $(TICKET) --output $@

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
# m1n1 keeps only the last initramfs it finds, so firmware cannot be a second
# payload. Linux reads cpio archives back to back in one stream, though, so the
# firmware cpios go on the end of Hoolock's, inside the same gzip.
FW_CPIOS = $(wildcard $(IBOOT)/mtfw.cpio $(IBOOT)/bcmfw.cpio)
BOOT_INITRAMFS = $(if $(FW_CPIOS),$(IBOOT)/initramfs-fw.gz,$(INITRAMFS))

$(IBOOT)/initramfs-fw.gz: $(INITRAMFS) $(FW_CPIOS)
	{ gzip -dc $(INITRAMFS); cat $(FW_CPIOS); } | gzip -9 > $@

$(KERNELCACHE): $(M1N1) $(DTB) $(IMAGE) $(BOOT_INITRAMFS) FORCE | $(IBOOT)
	{ cat $(M1N1); \
	  echo 'chosen.bootargs=$(BOOTARGS)'; \
	  cat $(DTB) $(IMAGE) $(BOOT_INITRAMFS); } > $(MACHO)
	ipsw img4 create --input $(MACHO) --type rkrn --compress lzss \
	    --extra $(MONITOR) --im4m $(IM4M) --output $@
	@echo "built $@ ($$(stat -c%s $@) bytes)"

# Phone in DFU on the jig, exploit fires by itself, move the cable here.
# iboot-boot.sh checks for the one-time images itself and says what is missing,
# so this does not depend on them and never goes looking for an IPSW.
boot: $(KERNELCACHE)
	./tools/iboot-boot.sh $(KERNELCACHE)

# Mesa's build-time compilers for the host, pinned to the commit
# patches/mesa-*.patch was made on. Needs clang, LLVM's SPIR-V translator and
# libclc; LLVM_DIR points at them.
MESA_SRC ?= build/mesa
MESA_REV := 24a0a3bddf5588d7f087e8f40658ba5e17af6094
LLVM_DIR ?= /usr/lib/llvm/22
MESA_BIN := $(MESA_SRC)/build/src

$(MESA_SRC)/build/build.ninja: $(wildcard patches/mesa-*.patch)
	test -d $(MESA_SRC) || { git init -q $(MESA_SRC) && \
	    git -C $(MESA_SRC) fetch -q --depth 1 https://gitlab.freedesktop.org/mesa/mesa.git $(MESA_REV) && \
	    git -C $(MESA_SRC) checkout -q FETCH_HEAD; }
	$(MAKE) patch
	test -d $(MESA_SRC)/build || PATH=$(LLVM_DIR)/bin:$$PATH \
	    PKG_CONFIG_PATH=$(LLVM_DIR)/lib64/pkgconfig meson setup $(MESA_SRC)/build $(MESA_SRC) \
	    -Dgallium-drivers= -Dvulkan-drivers= -Dplatforms= -Dglx=disabled -Degl=disabled \
	    -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dtools=asahi \
	    -Dmesa-clc=enabled -Dprecomp-compiler=enabled -Dllvm=enabled -Dbuildtype=debugoptimized
	touch $@

# Rusticl for the phone. apk.static builds two Alpine aarch64 trees without
# root: a sysroot with the -dev packages to build against, and the runtime
# rootfs. Mesa's build-time compilers come from the host build above.
ALPINE     := build/alpine
ALPINE_URL := https://dl-cdn.alpinelinux.org/alpine/v3.23
APK = $(ALPINE)/sbin/apk.static --arch aarch64 --usermode --allow-untrusted --no-scripts \
	-X $(ALPINE_URL)/main -X $(ALPINE_URL)/community
SYSROOT_PKGS := musl-dev gcc g++ libdrm-dev llvm21-dev clang21-dev clang21-libclang \
	spirv-llvm-translator-dev spirv-tools-dev libclc-dev expat-dev zlib-dev zstd-dev \
	elfutils-dev opencl-headers opencl-icd-loader-dev
ROOTFS_PKGS := alpine-baselayout busybox musl libdrm zlib zstd-libs expat spirv-tools \
	llvm21-libs clang21-libs spirv-llvm-translator-libs libstdc++ libgcc opencl-icd-loader strace
RUST_DIR  := build/rust
RUST_VER  := 1.98.1
RUSTC     := $(RUST_DIR)/rustup/toolchains/$(RUST_VER)-x86_64-unknown-linux-gnu/bin/rustc
MESA_A12  := $(MESA_SRC)/build-a12
A12_CC    := clang --target=aarch64-alpine-linux-musl --sysroot=$(ALPINE)/root -fuse-ld=lld
HOST_PATH := PKG_CONFIG_PATH=$(LLVM_DIR)/lib64/pkgconfig PATH=$(abspath $(dir $(RUSTC))):$(abspath $(MESA_BIN))/compiler/clc:$(abspath $(MESA_BIN))/asahi/clc:$(abspath $(MESA_BIN))/compiler/spirv:$(LLVM_DIR)/bin:$$PATH

rootfs: $(ALPINE)/rootfs.tar

$(ALPINE)/sbin/apk.static:
	mkdir -p $(ALPINE)
	curl -s $(ALPINE_URL)/main/x86_64/ | grep -o 'apk-tools-static-[^"]*\.apk' | head -1 | \
	    xargs -I{} curl -s -o $(ALPINE)/apk-tools-static.apk $(ALPINE_URL)/main/x86_64/{}
	tar xzf $(ALPINE)/apk-tools-static.apk -C $(ALPINE) sbin/apk.static

$(ALPINE)/root $(ALPINE)/rootfs: | $(ALPINE)/sbin/apk.static
	$(APK) --root $(abspath $@) --initdb --update-cache add \
	    $(if $(filter %/root,$@),$(SYSROOT_PKGS),$(ROOTFS_PKGS))

$(RUSTC):
	mkdir -p $(RUST_DIR)
	curl -s -o $(RUST_DIR)/rustup-init \
	    https://static.rust-lang.org/rustup/dist/x86_64-unknown-linux-gnu/rustup-init
	chmod +x $(RUST_DIR)/rustup-init
	RUSTUP_HOME=$(abspath $(RUST_DIR))/rustup CARGO_HOME=$(abspath $(RUST_DIR))/cargo \
	    $(RUST_DIR)/rustup-init -y --no-modify-path --profile minimal \
	    --default-toolchain $(RUST_VER) -t aarch64-unknown-linux-musl

$(ALPINE)/cross.ini: tools/alpine-cross.ini | $(ALPINE)/root
	sed -e 's|@SYSROOT@|$(abspath $(ALPINE)/root)|g' -e 's|@RUSTC@|$(abspath $(RUSTC))|g' \
	    -e 's|@TOOLS@|$(abspath tools)|g' $< > $@

$(ALPINE)/rootfs.tar: $(MESA_SRC)/build/build.ninja $(ALPINE)/cross.ini $(RUSTC) tools/cl-test.c tools/gl-test.c FORCE | $(ALPINE)/rootfs
	$(HOST_PATH) ninja -C $(MESA_SRC)/build src/compiler/clc/mesa_clc src/asahi/clc/asahi_clc \
	    src/compiler/spirv/vtn_bindgen2
	test -d $(MESA_A12) || $(HOST_PATH) meson setup $(MESA_A12) $(MESA_SRC) \
	    --cross-file $(ALPINE)/cross.ini -Dgallium-drivers=asahi -Dvulkan-drivers= \
	    -Dgallium-rusticl=true -Dllvm=enabled -Dshared-llvm=enabled -Dplatforms= \
	    -Dglx=disabled -Degl=enabled -Dgbm=enabled -Dgles1=disabled -Dgles2=enabled \
	    -Dopengl=true \
	    -Dmesa-clc=system -Dprecomp-compiler=system -Dmesa-clc-bundle-headers=enabled \
	    -Dstatic-libclc=all -Dbuildtype=release -Dprefix=/usr -Dlibdir=lib
	$(HOST_PATH) ninja -C $(MESA_A12)
	DESTDIR=$(abspath $(ALPINE)/rootfs) meson install -C $(MESA_A12) --no-rebuild --quiet --strip
	PATH=$(LLVM_DIR)/bin:$$PATH $(A12_CC) -O2 -Wall -o $(ALPINE)/rootfs/usr/bin/cl-test \
	    tools/cl-test.c -lOpenCL
	PATH=$(LLVM_DIR)/bin:$$PATH $(A12_CC) -O2 -Wall -o $(ALPINE)/rootfs/usr/bin/gl-test \
	    -I$(ALPINE)/rootfs/usr/include -L$(ALPINE)/rootfs/usr/lib tools/gl-test.c -lEGL -lGLESv2
	tar -C $(ALPINE)/rootfs --owner=0 --group=0 -cf $@ .

clean:
	rm -f $(DTB) $(PP) $(MACHO) $(KERNELCACHE)
