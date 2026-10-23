#!/bin/sh
# build.sh - build sde-split-hijack.ko against a kernel tree.
#
# Usage:
#   ./scripts/build.sh /path/to/kernel/tree
#
# The tree must be the one the device's kernel was built from, or at least one
# with a matching module ABI (vermagic + symbol CRCs). For piano that is a
# Xiaomi SM8750 tree carrying the display driver, not a plain GKI tree: the
# module needs the vendor headers and the display Kbuild environment.
#
# Two things bite when building out of tree on this kernel:
#
#   1. vermagic. scripts/setlocalversion appends -g<sha>-dirty from git, which
#      will not match the device. Pin UTS_RELEASE instead.
#
#   2. asm/compiler.h. arch/arm64/include/generated/asm/ is populated by
#      `make asm-generic`, but asm-generic/compiler.h does not exist, so the
#      symlink is never created. Add it by hand.
#
# Both are handled below and are idempotent.

set -eu

KDIR=${1:-}
if [ -z "$KDIR" ]; then
	echo "usage: $0 /path/to/kernel/tree" >&2
	exit 1
fi
if [ ! -d "$KDIR" ]; then
	echo "error: $KDIR is not a directory" >&2
	exit 1
fi

HERE=$(cd "$(dirname "$0")/.." && pwd)

# The vermagic the device reports for its modules. Override with VMAGIC=...
VMAGIC=${VMAGIC:-6.6.57-4k-g34b7c5063ce7}

echo "==> pinning UTS_RELEASE to $VMAGIC"
mkdir -p "$KDIR/include/generated"
printf '#define UTS_RELEASE "%s"\n' "$VMAGIC" \
	> "$KDIR/include/generated/utsrelease.h"

if [ ! -e "$KDIR/arch/arm64/include/generated/asm/compiler.h" ]; then
	echo "==> adding asm/compiler.h symlink"
	mkdir -p "$KDIR/arch/arm64/include/generated/asm"
	ln -sf ../../../asm/compiler.h \
		"$KDIR/arch/arm64/include/generated/asm/compiler.h"
fi

echo "==> building"
cd "$HERE"
# Clear stale objects so the new UTS_RELEASE is picked up.
rm -f ./*.o ./*.ko ./.??*.cmd ./*.cmd Module.symvers modules.order \
	./sde-split-hijack.mod ./sde-split-hijack.mod.c 2>/dev/null || true

make -C "$KDIR" M="$HERE" ARCH=arm64 LLVM=1 modules

echo
echo "==> result"
modinfo ./sde-split-hijack.ko | grep -E '^(name|vermagic|license)'
