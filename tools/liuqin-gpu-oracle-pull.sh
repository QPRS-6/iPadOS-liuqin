#!/bin/sh
# Push the read-only Android GPU oracle collector, run it with su, pull the .txt.
#
#   tools/liuqin-gpu-oracle-pull.sh [OUT.txt]
#
# Default output: gpu-oracle-<timestamp>.txt in the current directory.
# Requires adb and root on the Android side (Magisk/su).  The device script
# only reads sysfs/debugfs/procfs; nothing on the tablet is modified.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
out=${1:-gpu-oracle-$(date +%Y%m%d-%H%M%S).txt}
ADB=${ADB:-adb}
remote_dir=/data/local/tmp
remote=$remote_dir/gpu-oracle.txt

mkdir -p "$(dirname "$out")"

command -v "$ADB" >/dev/null 2>&1 || { echo "error: adb not found" >&2; exit 1; }
"$ADB" wait-for-device
if ! "$ADB" shell "su -c id" 2>/dev/null | grep -q 'uid=0'; then
	echo "error: no root shell (su -c id did not report uid=0)" >&2
	exit 1
fi

"$ADB" push "$here/gpu-oracle-android.sh" "$remote_dir/gpu-oracle-android.sh" >/dev/null
# OUT is fixed so the pull path is predictable; the script falls back to
# /data/local/tmp itself if /sdcard is not writable from the root context.
"$ADB" shell "su -c 'OUT=/sdcard/Download/gpu-oracle.txt sh $remote_dir/gpu-oracle-android.sh'"
if ! "$ADB" pull /sdcard/Download/gpu-oracle.txt "$out" >/dev/null 2>&1; then
	"$ADB" pull "$remote" "$out" >/dev/null
fi
echo "oracle: $out" >&2
