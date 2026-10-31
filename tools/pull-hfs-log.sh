#!/bin/bash
# Pull the kernel log ring from the hfs partition (sda36, 32 GiB) via recovery ADB.
#
#   tools/pull-hfs-log.sh /tmp/log.txt
#
# The ring lives in the last 4 MiB + 4 KiB of the partition.
set -euo pipefail
out=${1:-/tmp/hfs-log.txt}
here=$(cd "$(dirname "$0")" && pwd)
dev=${DEV:-/dev/block/sda36}
blocks=$((33554432 * 1024 / 4096))          # 32 GiB in 4 KiB blocks
skip=$((blocks - 1025))
adb exec-out "dd if=$dev bs=4096 skip=$skip count=1025 2>/dev/null" > "$out.bin"
python3 "$here/liuqin-log.py" "$out.bin" "$out"
echo "log: $out" >&2
