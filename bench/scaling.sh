#!/usr/bin/env bash
# bench/scaling.sh [OPTIONS] [WORKTREE [ROUND [PORT]]]      (bench/bench scaling-io; Spec 14 section 4)
#
# Compute, cold I/O and writer scaling of pg_sift on a throwaway cluster built from WORKTREE (default: this checkout); nothing is written into WORKTREE, no root, no Docker.
#   Suite A  warm compute, W = 1 -> 2 -> 4 -> 8 workers: A1 scan + filter, A2 low-cardinality GROUP BY, A3 high-cardinality GROUP BY, A4 count(DISTINCT), A5 parallel CREATE INDEX, A6 parallel VACUUM
#   Suite B  cold I/O, io_method io_uring / worker / sync x sift.io_read_stream on / off (release builds always stream: only the on rows) x W: B1 wide scan, B2 two of fifteen columns, B3 sift_table bitmap scan, B4 BM25 over 8 segments,
#            B5 a hot table survives a cold scan and an index build. Before every timed run the relation leaves shared_buffers (pg_buffercache_evict_relation) and the page cache
#            (posix_fadvise DONTNEED on its segment files); a run whose reads are under 90% of its blocks is marked `notcold`
#   Suite C  writers, C = 1 -> 4 -> 16 clients over 64 hash partitions (60% INSERT, 30% UPDATE, 10% DELETE) with a 10 ms sampler of wait events, FOR KEY SHARE without WAL, readers beside writers
# Options
#   --rows N         rows of the wide events table (default 2,000,000; --large 10,000,000; --quick 500,000 and workers 1,4 and clients 1,4,16 for 5 s)
#   --workers LIST   workers of suites A and B (default 1,2,4,8)      --clients LIST   clients of suite C (default 1,4,16)      --duration S   seconds of a suite C phase (default 15)
#   --suites LIST    A,B,C in any combination (default all)           --control   the same probes on the heap with no pg_sift (the stock executor as the reference)
#   --docs           also write docs/results/v6-scaling.tsv of this checkout (scripts/gen-docs.py reads it)
# Environment: BENCH_SCRATCH_ROOT (the cluster directory, default /tmp: suite B needs a disk, not tmpfs; BENCH_ALLOW_TMPFS=1 runs it anyway and measures the cache), BENCH_MAKE_JOBS (default 4)
# Output: bench/out/scaling-ROUND.tsv (suite probe io_method workers rows median_ms speedup blks_read blks_hit gate_pass read_stream throughput blk_read_ms p99_ms wal wait_pct, with `# key=value` meta lines)
#         and bench/out/scaling-ROUND.md (the gates). A gate on fewer than 8 workers or 16 clients is scaled by workers / 8 (clients / 16).
set -euo pipefail
export LC_ALL=C   # psql \watch and the timings read decimal points

BENCH_DIR=$(cd "$(dirname "$0")" && pwd)
. "$BENCH_DIR/steps/f2/cluster.sh"

ROWS=2000000 WORKERS=1,2,4,8 CLIENTS=1,4,16 DURATION=15 SUITES=A,B,C DOCS='' CONTROL='' QUICK=''
while (( $# )); do
	case $1 in
		--rows) ROWS=$2; shift ;;
		--large) ROWS=10000000 ;;
		--quick) ROWS=500000 WORKERS=1,4 DURATION=5 QUICK=1 ;;
		--workers) WORKERS=$2; shift ;;
		--clients) CLIENTS=$2; shift ;;
		--duration) DURATION=$2; shift ;;
		--suites) SUITES=${2^^}; shift ;;
		--control) CONTROL=1 ;;
		--docs) DOCS=1 ;;
		-h|--help) sed -n '2,23p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		--*) echo "scaling.sh: unknown option $1" >&2; exit 1 ;;
		*) break ;;
	esac
	shift
done
WORKTREE=${1:-$(dirname "$BENCH_DIR")}
ROUND=${2:-scaling-$(date -u +%Y%m%dT%H%M%S)}
PORT=${3:-${SIFT_PORT:-57641}}
OUT_DIR=$BENCH_DIR/out
RAW=$OUT_DIR/scaling-$ROUND.rows
PROBE=("$BENCH_DIR/steps/scaling_io.py")
[[ -z $CONTROL ]] || export V6_CONTROL=1

v6_cluster_init "$WORKTREE" "$ROUND" "$PORT"
mkdir -p "$OUT_DIR"
: >"$RAW"
[[ -n $CONTROL ]] || v6_cluster_build
v6_cluster_start
probe() { # COMMAND [OPTION...]: one command of steps/scaling_io.py against this cluster
	local cmd=$1; shift
	python3 "${PROBE[0]}" "$cmd" --host "$SOCK_DIR" --port "$PORT" --data-dir "$DATA_DIR" --rows "$ROWS" --workers "$WORKERS" --clients "$CLIENTS" --duration "$DURATION" --out "$RAW" ${CONTROL:+--control} "$@"
}
RESTART_ARGS() { # SHARED_BUFFERS [IO_METHOD]: the settings of a start; io_method exists from PostgreSQL 18
	printf '%s\n' "shared_buffers=$1" track_io_timing=on effective_io_concurrency=64 io_combine_limit=256kB
	[[ -z ${2:-} || $PG_MAJOR -lt 18 ]] || printf '%s\n' "io_method=$2"
}
restart() { mapfile -t args < <(RESTART_ARGS "$@"); v6_cluster_restart "${args[@]}"; }

echo "[scaling] round=$ROUND rows=$ROWS workers=$WORKERS clients=$CLIENTS suites=$SUITES pg=$PG_MAJOR${CONTROL:+ (control: no pg_sift)}" >&2
if [[ $SUITES == *B* && -z ${BENCH_ALLOW_TMPFS:-} ]]; then
	case $(stat -f -c %T "$DATA_DIR") in
		tmpfs|ramfs) echo "scaling: $DATA_DIR is on tmpfs, where a cold read does not exist: BENCH_SCRATCH_ROOT=DIR on a disk, or BENCH_ALLOW_TMPFS=1 to measure the cache" >&2; exit 1 ;;
	esac
fi
restart "${BENCH_SHARED_BUFFERS_WARM:-2GB}" worker
probe load
if [[ $SUITES == *A* ]]; then echo "[scaling] suite A: warm compute" >&2; probe suite-a; fi
if [[ $SUITES == *B* ]]; then
	echo "[scaling] suite B: cold I/O" >&2
	methods=(io_uring worker sync); (( PG_MAJOR >= 18 )) || methods=(-)
	for m in "${methods[@]}"; do
		if restart 64MB "$m"; then probe suite-b --io-method "$m"
		else echo "scaling: io_method $m does not start on this host (see $LOG_FILE): its rows are missing" >&2; v6_cluster_start; fi
	done
	restart 256MB worker
	probe suite-b5
	probe finish
fi
if [[ $SUITES == *C* ]]; then restart "${BENCH_SHARED_BUFFERS_WARM:-2GB}" worker; echo "[scaling] suite C: writers" >&2; probe suite-c; fi

storage=$(lsblk -no MODEL "$(findmnt -no SOURCE --target "$DATA_DIR")" 2>/dev/null | head -1 | sed 's/ *$//' || true)
HEADER=$(printf '# version=%s\n# git=%s\n# pg=%s\n# cpu=%s\n# storage=%s\n# date=%s\n' "$(v6_psql -Atc "SELECT coalesce((SELECT extversion FROM pg_extension WHERE extname = 'pg_sift'), 'control')")" "$GIT_SHA" \
	"$(v6_psql -Atc 'SHOW server_version')" "$(sed -n 's/^model name[[:space:]]*: //p' /proc/cpuinfo | head -1)" "${storage:-unknown}" "$(date -u +%F)")
TSV=$OUT_DIR/scaling-$ROUND.tsv
{ printf '%s\n' "$HEADER"; printf 'suite\tprobe\tio_method\tworkers\trows\tmedian_ms\tspeedup\tblks_read\tblks_hit\tgate_pass\tread_stream\tthroughput\tblk_read_ms\tp99_ms\twal\twait_pct\n'; cat "$RAW"; } >"$TSV"
rm -f "$RAW"
python3 - "$TSV" >"$OUT_DIR/scaling-$ROUND.md" <<'EOF'
import sys
lines = [l.rstrip('\n') for l in open(sys.argv[1])]
meta = [l[2:] for l in lines if l.startswith('# ')]
rows = [l.split('\t') for l in lines if not l.startswith('#')]
head, rows = rows[0], rows[1:]
print('# pg_sift scaling round\n\n' + ', '.join(meta) + '\n')
print('| ' + ' | '.join(head) + ' |\n|' + '---|' * len(head))
for r in rows:
    print('| ' + ' | '.join(r) + ' |')
failed = [r[1] + ' ' + (r[2] if r[2] != '-' else '') + ' w=' + r[3] for r in rows if r[9] in ('fail', 'notcold')]
print('\n**Gates:** ' + ('all pass' if not failed else 'not met: ' + '; '.join(failed)))
EOF
[[ -z $DOCS ]] || cp "$TSV" "$(dirname "$BENCH_DIR")/docs/results/v6-scaling.tsv"
echo "[scaling] wrote $TSV and $OUT_DIR/scaling-$ROUND.md${DOCS:+ and docs/results/v6-scaling.tsv}" >&2
grep -c . "$TSV" >/dev/null
awk -F'\t' '!/^#/ && NR > 1 && ($10 == "fail" || $10 == "notcold") {n++} END {exit n ? 1 : 0}' "$TSV" || { echo "[scaling] gates not met or runs not cold: see $OUT_DIR/scaling-$ROUND.md" >&2; exit 1; }
