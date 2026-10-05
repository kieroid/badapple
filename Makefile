# Build and sanity check the T8020 device tree without needing a kernel tree.
# The two headers under include/ are copies of the kernel ones, so this works
# offline.

BOARD ?= t8020-d331p

DTS := dts/$(BOARD).dts
DTB := build/$(BOARD).dtb
PP  := build/$(BOARD).dts.pp

CPPFLAGS := -nostdinc -Iinclude -Idts -undef -D__DTS__ -x assembler-with-cpp

.PHONY: check clean
check: $(DTB)

$(DTB): $(DTS) dts/t8020.dtsi
	mkdir -p build
	cpp $(CPPFLAGS) $< -o $(PP)
	dtc -I dts -O dtb -o $@ $(PP)
	@echo "built $@"

clean:
	rm -rf build
