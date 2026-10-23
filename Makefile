# sde-split-hijack
#
# Build against a kernel tree that matches the running kernel's module ABI.
#
#   make KDIR=/path/to/kernel/tree
#   make install DESTDIR=/usr/local/lib/piano
#
# KDIR has no useful default on a cross-build host; pass it explicitly.
#
# The source lives in src/, so the object list is prefixed and the build is
# driven with src=... to keep the tree layout clean.

obj-m += sde-split-hijack.o

sde-split-hijack-y := src/sde-split-hijack.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

DESTDIR ?= /usr/local/lib/piano

.PHONY: all modules clean install check

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=arm64 LLVM=1 modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install: modules
	install -d $(DESTDIR)
	install -m 0644 sde-split-hijack.ko $(DESTDIR)/

# Print what the module claims about itself, so vermagic mismatches are caught
# before pushing it to a device.
check: modules
	@modinfo sde-split-hijack.ko | grep -E '^(filename|license|vermagic|name)'
	@echo "--- undefined symbols ---"
	@readelf -sW sde-split-hijack.ko | awk '$$7 == "UND" { print $$8 }' | sort -u
