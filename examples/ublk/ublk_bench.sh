#!/bin/bash

# ublk server benchmark across commits
#
# Builds each given git revision in its own worktree, brings up the ublk
# server on top of polling (vector=-1) 1:1 SQ/CQ pairs, runs a set of fio
# workloads against /dev/ublkbN, and prints one table comparing all
# revisions.  Also records how many CPU cores unvmed (the ublk queue
# handler threads) consumed during each workload.
#
# By default it compares every commit in <base>..HEAD (base: origin/ublk),
# so each optimization commit can be measured on its own.

set -e

BDF=""
BASE="origin/ublk"
REVS=""
NR_QUEUES=1
QSIZE=256
DEPTH=128
RUNTIME=20
RAMP=3
UBLK_CPUS=""
FIO_CPUS=""
WRITES=0
OUTDIR=""
MESON_ARGS=""
FIO=${FIO:-fio}
REPO=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)

usage() {
	echo "Usage: $0 -b <bdf> [options]"
	echo "  -b <bdf>       : PCI BDF of the NVMe device (e.g., 0000:01:00.0)"
	echo "  -r \"<revs>\"    : Git revisions to compare (default: every commit in <base>..HEAD)"
	echo "  -B <base>      : Base for the default revision range (default: $BASE)"
	echo "  -n <nr_queues> : Number of I/O SQ/CQ pairs = ublk queues (default: $NR_QUEUES)"
	echo "  -z <qsize>     : NVMe SQ/CQ size (default: $QSIZE)"
	echo "  -D <depth>     : ublk queue depth (default: $DEPTH, clamped to qsize - 1)"
	echo "  -t <runtime>   : fio runtime per workload in seconds (default: $RUNTIME)"
	echo "  -c <cpus>      : Pin ublk handler threads, e.g. 2,3 (--cpus; revisions without the option ignore it)"
	echo "  -C <cpus>      : Pin fio jobs (fio --cpus_allowed), e.g. 4-7"
	echo "  -W             : Also run write workloads (DESTROYS DATA on the namespace)"
	echo "  -o <dir>       : Output directory (default: ./ublk-bench-<timestamp>)"
	echo "  -m \"<args>\"    : Extra 'meson setup' args, e.g. \"-Dwith-libvfn=/opt/libvfn\""
	echo ""
	echo "Environment: FIO=/path/to/fio (default: fio in PATH)"
	echo ""
	echo "Example: $0 -b 0000:01:00.0 -n 2 -c 2,3 -C 4-7"
	exit 1
}

while getopts "b:r:B:n:z:D:t:c:C:Wo:m:h" opt; do
	case ${opt} in
		b) BDF=$OPTARG ;;
		r) REVS=$OPTARG ;;
		B) BASE=$OPTARG ;;
		n) NR_QUEUES=$OPTARG ;;
		z) QSIZE=$OPTARG ;;
		D) DEPTH=$OPTARG ;;
		t) RUNTIME=$OPTARG ;;
		c) UBLK_CPUS=$OPTARG ;;
		C) FIO_CPUS=$OPTARG ;;
		W) WRITES=1 ;;
		o) OUTDIR=$OPTARG ;;
		m) MESON_ARGS=$OPTARG ;;
		*) usage ;;
	esac
done

if [ -z "$BDF" ]; then
	usage
fi

for tool in "$FIO" meson ninja python3; do
	if ! command -v "$tool" > /dev/null; then
		echo "Error: '$tool' not found"
		exit 1
	fi
done

if [ -z "$REVS" ]; then
	REVS=$(git -C "$REPO" rev-list --reverse "$BASE..HEAD")
fi
if [ -z "$REVS" ]; then
	echo "Error: no revisions to benchmark"
	exit 1
fi

OUTDIR=${OUTDIR:-$PWD/ublk-bench-$(date +%Y%m%d-%H%M%S)}
OUTDIR=$(realpath -m "$OUTDIR")
mkdir -p "$OUTDIR"
RESULTS="$OUTDIR/results.csv"
echo "idx,rev,subject,workload,iops,bw_mib,lat_mean_us,lat_p99_us,errors,unvmed_cores" > "$RESULTS"

# name  rw  bs  iodepth  numjobs
WORKLOADS=(
	"randread-4k-qd1     randread  4k   1  1"
	"randread-4k-qd32    randread  4k   32 $NR_QUEUES"
	"seqread-128k-qd16   read      128k 16 1"
)
if [ $WRITES -eq 1 ]; then
	WORKLOADS+=(
		"randwrite-4k-qd1  randwrite 4k   1  1"
		"randwrite-4k-qd32 randwrite 4k   32 $NR_QUEUES"
		"seqwrite-128k-qd16 write    128k 16 1"
	)
fi

UNVME=""

cleanup() {
	if [ -n "$UNVME" ] && pgrep -x unvmed > /dev/null; then
		"$UNVME" stop -a || true
	fi
}
trap cleanup EXIT

# CPU time (utime + stime, in clock ticks) consumed by unvmed so far.
unvmed_ticks() {
	local pid
	pid=$(pgrep -x unvmed | head -1)
	if [ -z "$pid" ]; then
		echo 0
		return
	fi
	awk '{ print $14 + $15 }' "/proc/$pid/stat"
}

build_rev() {
	local rev=$1 dir=$2

	if [ ! -d "$dir" ]; then
		git -C "$REPO" worktree add --detach "$dir" "$rev" > /dev/null
	fi
	if [ ! -d "$dir/build" ]; then
		# shellcheck disable=SC2086
		meson setup "$dir/build" "$dir" -Dbuildtype=release $MESON_ARGS \
			> "$dir/build.log" 2>&1
	fi
	ninja -C "$dir/build" >> "$dir/build.log" 2>&1
}

setup_ublk() {
	local depth_args=(-d "$DEPTH")
	local cpu_args=()

	if pgrep -x unvmed > /dev/null; then
		"$UNVME" stop -a
	fi

	"$UNVME" start
	"$UNVME" add "$BDF" --nr-ioqs="$NR_QUEUES"
	"$UNVME" create-adminq "$BDF" -s 32 -c 32
	"$UNVME" enable "$BDF"
	"$UNVME" id-ns "$BDF" -n 1 > /dev/null

	for ((qid = 1; qid <= NR_QUEUES; qid++)); do
		"$UNVME" create-iocq "$BDF" -q $qid -z "$QSIZE" -v -1
		"$UNVME" create-iosq "$BDF" -q $qid -z "$QSIZE" -c $qid
	done

	# Revisions before the --cpus option leave handler threads unpinned.
	if [ -n "$UBLK_CPUS" ] && grep -q '"cpus"' "$SRC_DIR/src/unvmed-cmds.c"; then
		cpu_args=(--cpus "$UBLK_CPUS")
	fi

	UBLK_OUT=$("$UNVME" ublk-server "$BDF" -n 1 "${depth_args[@]}" "${cpu_args[@]}")
	echo "$UBLK_OUT"
	UBLK_DEV=$(echo "$UBLK_OUT" | grep -o '/dev/ublkb[0-9]*' | head -1)
	if [ -z "$UBLK_DEV" ]; then
		echo "Error: failed to parse ublk device from: $UBLK_OUT"
		return 1
	fi

	for _ in $(seq 50); do
		[ -b "$UBLK_DEV" ] && return 0
		sleep 0.1
	done
	echo "Error: $UBLK_DEV did not appear"
	return 1
}

run_workload() {
	local idx=$1 rev=$2 subject=$3 name=$4 rw=$5 bs=$6 qd=$7 jobs=$8
	local json="$OUTDIR/$idx-$name.json"
	local fio_args=()
	local t0 t1 hz

	if [ -n "$FIO_CPUS" ]; then
		fio_args+=(--cpus_allowed="$FIO_CPUS")
	fi

	hz=$(getconf CLK_TCK)
	t0=$(unvmed_ticks)
	"$FIO" --name="$name" --filename="$UBLK_DEV" --ioengine=io_uring \
		--direct=1 --rw="$rw" --bs="$bs" --iodepth="$qd" \
		--numjobs="$jobs" --group_reporting --time_based \
		--ramp_time="$RAMP" --runtime="$RUNTIME" \
		--continue_on_error=all --output-format=json \
		"${fio_args[@]}" > "$json" || true
	t1=$(unvmed_ticks)

	python3 - "$json" "$idx" "$rev" "$subject" "$name" "$t0" "$t1" "$hz" \
		"$RUNTIME" "$RAMP" >> "$RESULTS" <<'EOF'
import json, sys
path, idx, rev, subject, name, t0, t1, hz, runtime, ramp = sys.argv[1:]
subject = subject.replace(',', ' ')
cores = (int(t1) - int(t0)) / int(hz) / (int(runtime) + int(ramp))
try:
    with open(path) as f:
        txt = f.read()
    job = json.loads(txt[txt.index('{'):])['jobs'][0]
except Exception:
    print(f"{idx},{rev},{subject},{name},FAIL,,,,,{cores:.2f}")
    sys.exit(0)
iops = bw = lat = p99 = 0.0
for d in ('read', 'write'):
    s = job[d]
    if s['io_bytes'] == 0:
        continue
    iops += s['iops']
    bw += s['bw'] / 1024
    lat = s['clat_ns']['mean'] / 1000
    p99 = s['clat_ns'].get('percentile', {}).get('99.000000', 0) / 1000
print(f"{idx},{rev},{subject},{name},{iops:.0f},{bw:.1f},{lat:.1f},{p99:.1f},"
      f"{job.get('total_err', 0)},{cores:.2f}")
EOF
	tail -1 "$RESULTS"
}

idx=0
for rev in $REVS; do
	short=$(git -C "$REPO" rev-parse --short "$rev")
	subject=$(git -C "$REPO" log -1 --format=%s "$rev")
	dir="$OUTDIR/src-$idx-$short"

	echo "=========================================="
	echo "[$idx] $short $subject"
	echo "=========================================="

	if ! build_rev "$rev" "$dir"; then
		echo "Build failed, see $dir/build.log"
		idx=$((idx + 1))
		continue
	fi
	UNVME="$dir/build/src/unvme"
	SRC_DIR="$dir"

	if ! setup_ublk > "$OUTDIR/$idx-setup.log" 2>&1; then
		echo "ublk setup failed, see $OUTDIR/$idx-setup.log"
		cleanup
		idx=$((idx + 1))
		continue
	fi
	echo "ublk device: $UBLK_DEV"

	for w in "${WORKLOADS[@]}"; do
		# shellcheck disable=SC2086
		run_workload $idx "$short" "$subject" $w
	done

	"$UNVME" ublk-stop "$BDF" || true
	"$UNVME" stop -a || true
	idx=$((idx + 1))
done

echo ""
echo "=========================================="
echo "Summary ($RESULTS)"
echo "=========================================="
python3 - "$RESULTS" <<'EOF'
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1])))
cols = ['idx', 'rev', 'workload', 'iops', 'bw_mib', 'lat_mean_us',
        'lat_p99_us', 'errors', 'unvmed_cores']
w = {c: max(len(c), *(len(r[c]) for r in rows)) for c in cols} if rows else {}
print('  '.join(c.ljust(w[c]) for c in cols))
for r in sorted(rows, key=lambda r: (r['workload'], int(r['idx']))):
    print('  '.join(r[c].ljust(w[c]) for c in cols))
print()
for i in sorted({(int(r['idx']), r['rev'], r['subject']) for r in rows}):
    print(f"[{i[0]}] {i[1]} {i[2]}")
EOF

for d in "$OUTDIR"/src-*; do
	git -C "$REPO" worktree remove --force "$d" > /dev/null 2>&1 || true
done
