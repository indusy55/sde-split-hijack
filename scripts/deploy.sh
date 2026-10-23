#!/bin/sh
# deploy.sh - push the module to a running device and load it.
#
# Works both from Android (adb shell) and from the Linux guest, since the guest
# is reachable over the same adb connection. On the guest the module is copied
# to /usr/local/lib/piano so piano-init loads it on the next handoff.
#
# Usage:
#   ./scripts/deploy.sh              # copy + insmod now
#   ./scripts/deploy.sh --persist    # copy only, for piano-init to pick up
#   ./scripts/deploy.sh --reload     # rmmod then insmod
#
# Environment:
#   ADB    adb binary (default: adb)
#   REMOTE directory on the device (default: /usr/local/lib/piano)

set -eu

ADB=${ADB:-adb}
REMOTE=${REMOTE:-/usr/local/lib/piano}
KO=sde-split-hijack.ko

mode=load
case "${1:-}" in
	--persist) mode=persist ;;
	--reload)  mode=reload ;;
	"")        ;;
	*) echo "usage: $0 [--persist|--reload]" >&2; exit 1 ;;
esac

HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE"

[ -f "$KO" ] || { echo "error: $KO not built; run scripts/build.sh first" >&2; exit 1; }

echo "==> pushing $KO to $REMOTE"
$ADB shell "su -c 'mkdir -p $REMOTE'" 2>/dev/null || true
$ADB push "$KO" "$REMOTE/" >/dev/null

if [ "$mode" = persist ]; then
	echo "==> copied; piano-init will load it on the next handoff"
	exit 0
fi

if [ "$mode" = reload ]; then
	echo "==> unloading"
	$ADB shell "su -c 'rmmod sde_split_hijack'" 2>/dev/null || true
fi

echo "==> loading"
$ADB shell "su -c '
	if grep -q \"^sde_split_hijack \" /proc/modules; then
		echo \"already loaded\"
	else
		insmod $REMOTE/$KO && echo loaded
	fi
'" 2>&1

echo "==> state"
$ADB shell "su -c '
	grep \"^sde_split_hijack \" /proc/modules 2>/dev/null || echo \"not loaded\"
	cat /sys/module/sde_split_hijack/parameters/enabled 2>/dev/null \
		| sed \"s/^/enabled=/\"
'" 2>&1
