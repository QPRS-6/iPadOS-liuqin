#!/system/bin/sh
# Extract the LIUQIN persistent console log from Android (root shell).
#
#   adb push tools/liuqin-log-android.sh /data/local/tmp/
#   adb shell su -c 'sh /data/local/tmp/liuqin-log-android.sh'
#
# The iPadOS kernel (patch 0047) keeps a 4 MiB ring at the end of the
# partition that holds the iPadOS root: the last 1025 4 KiB blocks.
#   block 0      header "LIUQINLO", u32 version, u32 ring size,
#                u64 total bytes, u64 flush sequence
#   blocks 1..   ring, byte n of the stream at ring[n % size]
#
# This script only reads the block device.  It needs root (Magisk/su);
# without root Android cannot open /dev/block/... .
#
# Output: /sdcard/Download/liuqin-log-<timestamp>.txt
#         and a short header line on stderr.
#
# If the device has no hfs by-name link, pass it explicitly:
#   adb shell su -c 'DEV=/dev/block/sda36 sh /data/local/tmp/liuqin-log-android.sh'

set -eu
export LC_ALL=C

OUTDIR=${OUTDIR:-/sdcard/Download}
STAMP=$(date +%Y%m%d-%H%M%S 2>/dev/null || date +%s)
OUT=${OUT:-$OUTDIR/liuqin-log-$STAMP.txt}
TMP=${TMPDIR:-/data/local/tmp}/liuqin-log.$$
AREA=$TMP/area.bin
RING=$TMP/ring.bin

cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT INT HUP TERM

if [ "$(id -u)" != "0" ]; then
	echo "error: run as root (adb shell su -c 'sh $0')" >&2
	exit 1
fi

# The port puts the iPadOS root on "hfs" (it used to be "userdata").
# Prefer by-name; the raw sda36 fallback is the 32 GiB hfs LUN on this board.
if [ -z "${DEV:-}" ]; then
	for p in \
		/dev/block/by-name/hfs \
		/dev/block/bootdevice/by-name/hfs \
		/dev/block/platform/soc/1d84000.ufshc/by-name/hfs \
		/dev/block/by-name/userdata \
		/dev/block/bootdevice/by-name/userdata \
		/dev/block/platform/soc/1d84000.ufshc/by-name/userdata \
		/dev/block/sda36; do
		if [ -e "$p" ]; then DEV=$p; break; fi
	done
fi
[ -n "${DEV:-}" ] || {
	echo "error: hfs/userdata block device not found; set DEV=/dev/block/..." >&2
	exit 1
}

# Size in bytes.  toybox blockdev has --getsize64; fall back to 512-byte sectors.
bytes=$(blockdev --getsize64 "$DEV" 2>/dev/null || true)
if [ -z "$bytes" ] || [ "$bytes" -le 0 ] 2>/dev/null; then
	sectors=$(blockdev --getsz "$DEV" 2>/dev/null || true)
	[ -n "$sectors" ] || { echo "error: cannot size $DEV" >&2; exit 1; }
	bytes=$((sectors * 512))
fi
blocks=$((bytes / 4096))
[ "$blocks" -gt 1025 ] || { echo "error: $DEV is too small ($bytes bytes)" >&2; exit 1; }
skip=$((blocks - 1025))

mkdir -p "$TMP"
mkdir -p "$OUTDIR" 2>/dev/null || true

# Header block + ring.  Only reads: no partition is written.
dd if="$DEV" of="$AREA" bs=4096 skip="$skip" count=1025 2>/dev/null

magic=$(od -An -t c -N 8 "$AREA" | tr -d ' \n')
if [ "$magic" != "LIUQINLO" ]; then
	echo "error: no LIUQINLOG header at $DEV+$((skip * 4096)) (log never flushed, or wrong partition)" >&2
	echo "       first bytes: $(od -An -t x1 -N 16 "$AREA" | tr -s ' ')" >&2
	exit 1
fi

version=$(od -An -t u4 -j 8  -N 4 "$AREA" | tr -d ' \n')
size=$(od -An -t u4 -j 12 -N 4 "$AREA" | tr -d ' \n')
total=$(od -An -t u8 -j 16 -N 8 "$AREA" | tr -d ' \n')
seq=$(od -An -t u8 -j 24 -N 8 "$AREA" | tr -d ' \n')

case "$version:$size:$total:$seq" in
	*[!0-9:]*|:*|*::*)
		echo "error: unreadable header (version=$version size=$size total=$total seq=$seq)" >&2
		exit 1 ;;
esac
[ "$size" -gt 0 ] || { echo "error: zero ring size" >&2; exit 1; }
# The area holds at most 4 MiB of ring; clamp so a corrupt header cannot
# make us read past the file.
maxring=$((1025 * 4096 - 4096))
[ "$size" -le "$maxring" ] || size=$maxring

dd if="$AREA" of="$RING" bs=4096 skip=1 count=$((size / 4096)) 2>/dev/null

# The stream wraps at total % size.  head/tail work on bytes and avoid a
# 4-million-iteration `dd bs=1`.
emit_log() {
	if [ "$total" -le "$size" ]; then
		head -c "$total" "$RING"
	else
		start=$((total % size))
		tail -c +$((start + 1)) "$RING"
		head -c "$start" "$RING"
	fi
}
if ! emit_log | tr -d '\000\r' > "$OUT" 2>/dev/null; then
	# /sdcard may be a FUSE mount the root context cannot write.
	OUT=/data/local/tmp/liuqin-log-$STAMP.txt
	emit_log | tr -d '\000\r' > "$OUT"
fi

echo "device=$DEV size=$bytes skip=$skip version=$version ring=$size total=$total seq=$seq" >&2
echo "log=$OUT ($(wc -c < "$OUT") bytes)" >&2
