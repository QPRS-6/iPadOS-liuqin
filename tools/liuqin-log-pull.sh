#!/bin/sh
# Read the LIUQIN console log (kernel patch 0047) from a phone booted into
# OrangeFox recovery (fastboot boot recovery/recovery.img; adb root shell).
#   tools/liuqin-log-pull.sh [-s SERIAL] OUT.txt
# Only reads: the last 1025 blocks (4 KiB) of the userdata partition.
set -eu

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
	echo "Usage: $(basename "$0") [-s SERIAL] OUT.txt"
	echo "Pull the 4 MiB kernel log ring from userdata via recovery ADB."
	exit 0
fi

ADB="adb"
if [ "${1:-}" = "-s" ]; then
	shift
	SERIAL=${1:?Error: -s requires a device serial}
	shift
	ADB="adb -s $SERIAL"
fi

out=${1:?usage: liuqin-log-pull.sh [-s SERIAL] OUT.txt}
command -v adb >/dev/null 2>&1 || { echo "Error: adb not found in PATH" >&2; exit 1; }

# The root lives on the "hfs" partition now (it used to be "userdata").  Recovery's
# toybox readlink has no -f, so ask it for the plain link target; if by-name is
# missing entirely, fall back to a device node given in $DEV.
dev=${DEV:-}
if [ -z "$dev" ]; then
	dev=$($ADB shell 'for p in /dev/block/by-name/hfs /dev/block/bootdevice/by-name/hfs /dev/block/by-name/userdata /dev/block/bootdevice/by-name/userdata; do if [ -e $p ]; then t=$(readlink $p 2>/dev/null); echo ${t:-$p}; exit 0; fi; done' 2>/dev/null | tr -d '\r' | head -1)
fi
[ -n "$dev" ] || { echo "hfs/userdata block device not found on connected device" >&2; exit 1; }
bytes=$($ADB shell blockdev --getsize64 "$dev" | tr -d '\r')
skip=$((bytes / 4096 - 1025))
$ADB exec-out "dd if=$dev bs=4096 skip=$skip count=1025 2>/dev/null" > "$out.bin"
echo "$dev: $bytes bytes, read blocks $skip.. ($(stat -c %s "$out.bin") bytes)" >&2
python3 "$(dirname "$0")/liuqin-log.py" "$out.bin" "$out"
