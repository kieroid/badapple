# check      compile the device tree on its own, no kernel tree needed
# deps       clone m1n1, the Hoolock kernel and the docs into build/
# pmgr       regenerate dts/t8020-pmgr.dtsi from the IPSW
# m1n1       build the m1n1 chainload payload
# kernel     build Image.gz with Hoolock's 16K config
# initramfs  download Hoolock's test initramfs
# blob       concatenate m1n1, the device tree, the kernel and the initramfs
# pongo      drive the phone from DFU to a PongoOS shell with l41ka
# boot       send the blob to the PongoOS shell and chainload it
# boot-m1n1  send m1n1 alone, leaving it in its USB proxy
# proxy-boot load kernel, dtb and initramfs over m1n1's proxy (cable on the PC)
# proxy-guest run the blob under m1n1's hypervisor, guest console on the host
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

.PHONY: check deps pmgr m1n1 kernel initramfs blob pongo boot boot-m1n1 clean
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

# l41ka is the only thing that gets an A12 to PongoOS: it runs usbliter8 from
# the RP2350 and embeds a PongoOS fork with T8020 support, which upstream
# PongoOS does not have. Its laikadbg.py speaks to the board over USB
# 4141:4c41, so the board has to be plugged into this machine with the phone
# connected through it.
L41KA ?= build/l41ka
LAIKA ?= build/venv/bin/python $(L41KA)/scripts/laikadbg.py

# Do not run the stages as separate laikadbg commands. Opening the board calls
# set_configuration(), which re-enumerates it and drops the downstream link, so
# a one-shot "exploit" fires at nothing, returns 0, and leaves pwned false.
# tools/chain.py holds one session open across all the stages instead.
pongo:
	build/venv/bin/python tools/chain.py pongo

boot: $(BLOB)
	build/venv/bin/python tools/chain.py boot $(BLOB)

# Sends m1n1 on its own, with nothing appended, so it ends up in uartproxy_run
# instead of handing off. Two uses: it proves m1n1 runs on T8020 at all, and it
# is the first half of the proxy workflow below.
boot-m1n1: $(M1N1)
	build/venv/bin/python tools/chain.py boot $(M1N1)

# The fast iteration loop, which avoids the DFU button dance per attempt.
#
#   make boot-m1n1          through the jig, leaves m1n1 sitting in its proxy
#   move the Lightning cable from the jig to this PC
#   make proxy-boot         loads kernel, dtb and initramfs over USB
#
# The point is that m1n1's log arrives as text on the host rather than having
# to be photographed off the screen, and a failed attempt can be retried
# without re-running the exploit, as long as m1n1 itself survives.
# Pick the m1n1 proxy port by USB vendor id rather than guessing ttyACM0,
# which is the l41ka board. m1n1's proxy is 1209:316d and exposes two ACM
# interfaces; the first one is the proxy.
M1N1DEV ?= $(shell for d in /sys/class/tty/ttyACM*; do \
	p=$$(readlink -f $$d/device); \
	[ "$$(cat $$p/../idVendor 2>/dev/null)" = "1209" ] && \
	    echo /dev/$$(basename $$d) && break; done)
PROXY := M1N1DEVICE=$(M1N1DEV) build/venv/bin/python $(M1N1_SRC)/proxyclient

.PHONY: proxy-boot proxy-shell
proxy-boot: $(IMAGE) $(DTB) $(INITRAMFS)
	$(PROXY)/tools/linux.py $(IMAGE) $(DTB) $(INITRAMFS) -b '$(BOOTARGS)'

proxy-shell:
	$(PROXY)/tools/shell.py

# The actual fix for "Linux prints nothing". Runs the blob as a guest under
# m1n1's hypervisor, which traps the guest's UART writes and prints them on the
# host, so earlycon output becomes readable without a serial cable on the
# Lightning pins. --hook-exceptions reports guest faults with registers.
# A12 implements EL2, which the ADT confirms: /product has-virtualization = 1.
.PHONY: proxy-guest
proxy-guest: $(BLOB)
	$(PROXY)/tools/run_guest.py -r -e -l build/guest.log $(BLOB)

clean:
	rm -f $(DTB) $(PP) $(BLOB)
