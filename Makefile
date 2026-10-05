# check      compile the device tree on its own, no kernel tree needed
# deps       clone m1n1, the Hoolock kernel and the docs into build/
# pmgr       regenerate dts/t8020-pmgr.dtsi from the IPSW
# m1n1       build the m1n1 chainload payload
# kernel     build Image.gz with Hoolock's 16K config
# initramfs  download Hoolock's test initramfs
# blob       concatenate m1n1, the device tree, the kernel and the initramfs
# boot       send the blob to a device already sitting in the PongoOS shell
#
# "make check" is the only target that works on a bare checkout. The headers
# under include/ are copies of the kernel ones so it needs no kernel tree.

BOARD ?= t8020-d331p

DTS := dts/$(BOARD).dts
DTB := build/$(BOARD).dtb
PP  := build/$(BOARD).dts.pp

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
BOOTARGS ?= earlycon console=tty0 loglevel=7 maxcpus=1

.PHONY: check deps pmgr m1n1 kernel initramfs blob boot clean
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

$(IMAGE):
	cp $(CONFIG_16K) $(KDIR)/.config
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

# Expects the phone to already be in the PongoOS shell by way of usbliter8.
boot: $(BLOB)
	printf '/send %s\nbootm\n' "$(BLOB)" | pongoterm

clean:
	rm -f $(DTB) $(PP) $(BLOB)
