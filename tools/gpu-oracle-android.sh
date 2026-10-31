#!/system/bin/sh
# Read-only GPU state collector for the Android side ("oracle").
#
#   adb push tools/gpu-oracle-android.sh /data/local/tmp/
#   adb shell su -c 'sh /data/local/tmp/gpu-oracle-android.sh'
#
# Output: /sdcard/Download/gpu-oracle.txt (falls back to /data/local/tmp).
# The script only reads sysfs/debugfs/procfs.  It never writes a kernel
# interface, never touches a partition, and never forces the GPU awake or
# triggers a postmortem dump.  (ORACLE_DEEP=1 additionally reads the
# register/snapshot files if the kernel exposes them; still read-only.)
#
# Android 12 / 5.10 note: kgsl does NOT create a "registers" debugfs file.
# Its debugfs is only globals + snapshot/break_isdb; a full register/ring
# snapshot exists only after a real GPU fault (devcoredump).  So this script
# reports what the kernel actually exposes instead of assuming the old path.
#
# Why: the XNU port's A2D/RB/SP blocks never answer.  On the same silicon the
# Android kgsl driver brings them up, so its post-init state is the reference
# to diff our register dump against.

set -u
export LC_ALL=C
PATH=/system/bin:/vendor/bin:/apex/com.android.runtime/bin:$PATH

OUT=${OUT:-/sdcard/Download/gpu-oracle.txt}
CAP=${CAP:-65536}          # per-file cap
mkdir -p "${OUT%/*}" 2>/dev/null || true
if ! { : > "$OUT"; } 2>/dev/null; then
	OUT=/data/local/tmp/gpu-oracle.txt
	: > "$OUT"
fi
exec 3>>"$OUT"

emit() { printf '%s\n' "$*" >&3 2>/dev/null || true; }

dump_file() {
	_p=$1; _c=${2:-$CAP}
	[ -e "$_p" ] || return 0
	[ -r "$_p" ] || { emit "--- $_p: not readable"; return 0; }
	_sz=$(stat -c %s "$_p" 2>/dev/null || echo '?')
	emit ""
	emit "=== $_p (size=$_sz) ==="
	head -c "$_c" "$_p" 2>/dev/null | tr -d '\000\r' >&3
	emit ""
	case "$_sz" in
	''|*[!0-9]*) ;;
	*) [ "$_sz" -gt "$_c" ] && emit "... [truncated at $_c of $_sz bytes]" ;;
	esac
}

dump_dir() {
	_dir=$1; _max=$2; _c=${3:-8192}
	[ -d "$_dir" ] || return 0
	find "$_dir" -maxdepth "$_max" -type f 2>/dev/null | head -n 400 |
	while IFS= read -r _f; do
		case "$_f" in
		*/devcoredump|*/snapshot|*/trace|*/trace_pipe|*/data) continue ;;
		esac
		dump_file "$_f" "$_c"
	done
}

list_tree() {
	_root=$1; _max=$2
	[ -d "$_root" ] || return 0
	emit ""
	emit "=== tree $_root (maxdepth $_max) ==="
	find "$_root" -maxdepth "$_max" 2>/dev/null | head -n 400
}

dt_strings() {
	if command -v strings >/dev/null 2>&1; then
		strings "$1" 2>/dev/null
	else
		tr -c '[:print:]\n' '\n' < "$1" 2>/dev/null
	fi
}

# DT cells are big-endian; od -tx4 prints the little-endian host view, so the
# byte-swapped value is expected.  Both views are dumped to avoid ambiguity.
dump_dt_prop() {
	_p=$1
	[ -f "$_p" ] || return 0
	emit ""
	emit "--- dt ${_p#/sys/firmware/devicetree/base}"
	dt_strings "$_p" | head -n 24
	od -An -tx1 -v "$_p" 2>/dev/null | head -n 24
}

emit "LIUQIN GPU ORACLE (Android side, read-only)"
emit "date=$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || date)"
emit "uname=$(uname -a 2>/dev/null)"
emit "boot_id=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)"
for _p in ro.product.device ro.product.model ro.build.fingerprint \
	ro.build.version.release ro.board.platform ro.soc.manufacturer \
	ro.soc.model ro.hardware; do
	emit "prop $_p=$(getprop "$_p" 2>/dev/null)"
done
dump_file /proc/version
dump_file /proc/cmdline
dump_file /proc/iomem 32768

# ---------------------------------------------------------------- kgsl sysfs
K=/sys/class/kgsl/kgsl-3d0
emit ""
emit "########## kgsl sysfs ##########"
list_tree "$K" 2
dump_dir "$K" 1 8192
dump_dir "$K/devfreq" 1 8192
dump_dir "$K/power" 1 4096
for _f in devfreq/cur_freq devfreq/governor devfreq/available_frequencies \
	devfreq/available_governors power/control power/runtime_status \
	power/runtime_suspended_time; do
	dump_file "$K/$_f" 8192
done

# --------------------------------------------------------------- kgsl debugfs
emit ""
emit "########## kgsl debugfs ##########"
for _d in /sys/kernel/debug/kgsl /d/kgsl; do
	[ -d "$_d" ] || continue
	list_tree "$_d" 3
	dump_dir "$_d/kgsl-3d0" 1 65536
	dump_dir "$_d/kgsl-3d0/postmortem" 1 4096
done
# files a modern kgsl may expose with register state
for _f in registers gpu_regs regs rbbm hwcg ifpc pwrup ucode lm preempt \
	faults active_contexts hang; do
	dump_file "$K/$_f" 65536
	dump_file "/sys/kernel/debug/kgsl/kgsl-3d0/$_f" 65536
done

# ------------------------------------------------------- devcoredump (stale)
# A snapshot from an earlier GPU fault is a full register/ring dump; reading
# one does not create a fault.
emit ""
emit "########## devcoredump (existing snapshots only) ##########"
for _d in /sys/class/devcoredump/devcd* /sys/kernel/debug/kgsl/kgsl-3d0; do
	[ -e "$_d" ] || continue
	[ -f "$_d/data" ] && dump_file "$_d/data" 262144
	[ -f "$_d/devcoredump" ] && dump_file "$_d/devcoredump" 262144
done

# ------------------------------------------------------------- device tree
emit ""
emit "########## device tree (GPU node) ##########"
for _base in /sys/firmware/devicetree/base /proc/device-tree; do
	[ -d "$_base" ] || continue
	emit ""
	emit "=== devicetree $_base ==="
	_node=$(find "$_base" -maxdepth 3 -type d -name 'gpu@*' 2>/dev/null | head -n 1)
	[ -n "${_node:-}" ] || { emit "(no gpu@ node)"; continue; }
	emit "gpu_node=$_node"
	find "$_node" -maxdepth 4 -type f 2>/dev/null | head -n 300 |
	while IFS= read -r _f; do dump_dt_prop "$_f"; done
done

# ------------------------------------------------- clock / power summaries
emit ""
emit "########## clocks, regulators, power domains ##########"
emit ""
emit "=== clk_summary (gpu/gfx/gmu/kgsl lines) ==="
grep -i -E 'gpu|gfx|gmu|kgsl|axon' /sys/kernel/debug/clk/clk_summary 2>/dev/null | head -n 200
emit ""
emit "=== regulator_summary (gpu lines) ==="
grep -i -E 'gpu|gfx|gmu' /sys/kernel/debug/regulator/regulator_summary 2>/dev/null | head -n 100
emit ""
emit "=== pm_genpd_summary (gpu lines) ==="
grep -i -E 'gpu|gfx|gmu' /sys/kernel/debug/pm_genpd/pm_genpd_summary 2>/dev/null | head -n 100
emit ""
emit "=== interconnect_summary (gpu lines) ==="
grep -i -E 'gpu|gfx|gmu' /sys/kernel/debug/interconnect/interconnect_summary 2>/dev/null | head -n 100

# --------------------------------------------------------------------- dmesg
emit ""
emit "########## dmesg ##########"
emit "=== dmesg (last 5000 lines) ==="
dmesg 2>/dev/null | tail -n 5000
emit ""
emit "=== dmesg kgsl/adreno/gpu/gmu lines ==="
dmesg 2>/dev/null | grep -i -E 'kgsl|adreno|gpu|gmu|a730' | tail -n 800

# ------------------------------------------------------ tracefs (optional)
# kgsl_regwrite carries (register offset, value).  It is the only way to see
# the kernel driver's own writes; it cannot show the userspace PM4 stream.
# Enabling it writes only to tracefs controls, and only with ORACLE_TRACE=1.
emit ""
emit "########## tracefs (kgsl tracepoints) ##########"
TRACE=
for _t in /sys/kernel/debug/tracing /sys/kernel/tracing; do
	[ -d "$_t" ] && { TRACE=$_t; break; }
done
if [ -z "${TRACE:-}" ]; then
	emit "(no tracefs mounted)"
else
	emit "tracefs=$TRACE"
	emit "=== available kgsl events ==="
	grep -E '^kgsl_' "$TRACE/available_events" 2>/dev/null | head -n 100
	if [ "${ORACLE_TRACE:-0}" = "1" ] &&
		[ -e "$TRACE/events/kgsl/kgsl_regwrite/enable" ]; then
		emit "=== kgsl_regwrite trace (3 s; run a GPU workload now) ==="
		: > "$TRACE/trace" 2>/dev/null
		echo 1 > "$TRACE/events/kgsl/kgsl_regwrite/enable" 2>/dev/null
		sleep 3
		echo 0 > "$TRACE/events/kgsl/kgsl_regwrite/enable" 2>/dev/null
		head -n 4000 "$TRACE/trace" 2>/dev/null
	fi
fi

# ------------------------------------------------- register access (probe only)
# No MMIO read is performed here.  A GPU MMIO read while the GX domain is
# powered down can hang the bus, so the controlled /dev/mem pass is a separate,
# explicitly approved step (needs the GPU base from the DT dump above).
emit ""
emit "########## register access (detection only, no MMIO reads) ##########"
emit "devmem=$(command -v devmem 2>/dev/null || echo none)"
ls -l /dev/mem 2>/dev/null || emit "/dev/mem: absent"

# -------------------------------------------------------------- opt-in deep
if [ "${ORACLE_DEEP:-0}" = "1" ]; then
	emit ""
	emit "########## ORACLE_DEEP=1: register/snapshot files ##########"
	for _f in registers gpu_regs regs rbbm hwcg ifpc pwrup snapshot; do
		dump_file "$K/$_f" 262144
		dump_file "/sys/kernel/debug/kgsl/kgsl-3d0/$_f" 262144
	done
fi

emit ""
emit "oracle_end"
exec 3>&-
chmod 0644 "$OUT" 2>/dev/null || true
echo "oracle=$OUT bytes=$(wc -c < "$OUT" 2>/dev/null)" >&2
echo "$OUT"
