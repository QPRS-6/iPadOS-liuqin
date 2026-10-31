#!/bin/sh
# Push tools/liuqin-log-android.sh to a booted Android device, run it with su,
# and pull the decoded text to the host.
#
#   tools/liuqin-log-android-pull.sh [OUT.txt]
#
# Requires adb and root on the Android side (Magisk/su).  The device script
# only reads the hfs partition; it does not format or mount it.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
out=${1:-liuqin-log-android.txt}
ADB=${ADB:-adb}
remote=/data/local/tmp/liuqin-log.txt

command -v "$ADB" >/dev/null 2>&1 || { echo "error: adb not found" >&2; exit 1; }
"$ADB" push "$here/liuqin-log-android.sh" /data/local/tmp/liuqin-log-android.sh >/dev/null
"$ADB" shell "su -c 'OUT=$remote sh /data/local/tmp/liuqin-log-android.sh'"
"$ADB" pull "$remote" "$out"
echo "log: $out" >&2
