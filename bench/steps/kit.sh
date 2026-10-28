#!/usr/bin/env bash
# The laptop kit (bench/bench kit): PostgreSQL with pg_sift in one docker container, the store data of the analytics benchmark, the ten
# questions timed with and without the aggregate pushdown, and every answer compared with the other one. Needs docker and nothing else.
#
#   bench/bench kit [--rows N] [--pg MAJOR] [--runs N] [--cpus N] [--image IMAGE] [--keep]
#
#   --rows N      orders (default 100000: the run itself takes under a minute; the first run also builds the image, which takes minutes)
#   --pg MAJOR    the PostgreSQL major of the image (default 18; packaging/docker/build.sh builds it)
#   --runs N      timed runs per question and mode after one warm-up (default 5; the table has the median)
#   --cpus N      CPU quota of the container (default: the whole host)
#   --image I     use this image instead of building pg_sift:pg<MAJOR>
#   --keep        leave the container running (name and psql line are printed)
# Exit status 3 when an answer with the pushdown differs from the one without it.
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
SQL=$HERE/sql/store
rows=100000 pg=18 runs=5 cpus='' image='' keep=''
while (( $# )); do
	case $1 in
		--rows) rows=$2; shift ;; --pg) pg=$2; shift ;; --runs) runs=$2; shift ;;
		--cpus) cpus=$2; shift ;; --image) image=$2; shift ;; --keep) keep=1 ;;
		-h|--help) sed -n '2,/^set -/p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 0 ;;
		*) echo "kit: unknown option $1 (bench/bench kit --help)" >&2; exit 1 ;;
	esac
	shift
done
command -v docker >/dev/null || { echo "kit: docker is required" >&2; exit 1; }

name=sift-kit-$$
log() { printf '%s\n' "$*" >&2; }
cleanup() { [[ -n $keep ]] || docker rm -f -v "$name" >/dev/null 2>&1 || true; }
trap cleanup EXIT

if [[ -z $image ]]; then
	log "== image: packaging/docker/build.sh $pg"
	"$HERE/../packaging/docker/build.sh" "$pg" >&2
	image=pg_sift:pg$pg
fi

log "== start $image"
docker run -d --name "$name" ${cpus:+--cpus "$cpus"} --shm-size 512m -e POSTGRES_PASSWORD=kit -e POSTGRES_DB=store \
	"$image" -c shared_buffers=512MB -c jit=off -c max_parallel_workers_per_gather=2 >/dev/null
for _ in $(seq 60); do docker exec "$name" pg_isready -q -h 127.0.0.1 -U postgres && break; sleep 1; done
docker exec "$name" pg_isready -q -h 127.0.0.1 -U postgres || { log "kit: the server did not start"; docker logs "$name" >&2; exit 1; }
q() { docker exec -i -e PGOPTIONS='-c client_min_messages=warning' "$name" psql -X -q -At -v ON_ERROR_STOP=1 -U postgres -d store "$@"; }
section() { awk -v s="$2" '/^--@ /{on = ($2 == s); next} on' "$1"; }   # the `--@ NAME` sections of the files of sql/store

log "== data: $rows orders"
t0=$SECONDS
q -v n="$rows" < "$SQL/gen.sql"
section "$SQL/sift.sql" table | q
q -c 'INSERT INTO orders SELECT * FROM orders_src ORDER BY id'
t1=$SECONDS
section "$SQL/sift.sql" indexes | q
q -c 'DROP TABLE orders_src' -c 'VACUUM ANALYZE orders'
t2=$SECONDS
sizes=$(q -F ' ' -c "SELECT pg_size_pretty(pg_table_size('orders')), pg_size_pretty(pg_relation_size('orders_col')), pg_size_pretty(pg_relation_size('orders_title'))")

# the statement of a question: its sift form if it has one, else the plain one
statement() { local s; s=$(section "$1" sift); [[ -n $s ]] || s=$(section "$1" pg); echo "${s//\{point_id\}/$(( rows * 42 / 100 ))}"; }
median() { sort -n | awk '{a[NR] = $1} END {print a[int((NR + 1) / 2)]}'; }
# timed MODE SQL: the median of RUNS runs in ms (psql \timing, one warm-up run first)
timed() {
	{ printf "SET statement_timeout = '60s';\nSET sift.enable_agg = %s;\n\\\\timing on\n\\\\o /dev/null\n" "$1"
	  for _ in $(seq $((runs + 1))); do printf '%s;\n' "$2"; done; } | q | sed -n 's/^Time: \([0-9.]*\) ms.*/\1/p' | tail -n "$runs" | median
}
# answer MODE SQL: the rows, sorted unless the question has an ORDER BY
answer() {
	local out; out=$(printf 'SET sift.enable_agg = %s;\n%s;\n' "$1" "$2" | q)
	if [[ $2 == *"ORDER BY"* ]]; then echo "$out"; else LC_ALL=C sort <<<"$out"; fi
}

log "== ten questions, median of $runs runs"
fail=0
printf '| question | label | pushdown (ms) | sift.enable_agg=off (ms) | ratio | plan | same answer |\n|---|---|---:|---:|---:|---|---|\n'
mapfile -t files < <(grep -H '^-- order: ' "$SQL"/queries/*.sql | sort -t: -k3 -n | cut -d: -f1)
for f in "${files[@]}"; do
	id=$(basename "$f" .sql) label=$(sed -n 's/^-- label: //p' "$f"); stmt=$(statement "$f")
	plan=$(printf 'SET sift.enable_agg = on;\nEXPLAIN (COSTS OFF) %s;\n' "$stmt" | q | grep -o -m1 'Custom Scan (SiftAgg)\|SiftScan\|SiftCount\|Index Scan\|Seq Scan\|Bitmap Heap Scan' | head -1 || true)
	if [[ $(answer on "$stmt") == "$(answer off "$stmt")" ]]; then same=yes; else same=DIFFERENT; fail=1; fi
	on=$(timed on "$stmt") off=$(timed off "$stmt")
	printf '| %s | %s | %s | %s | %s | %s | %s |\n' "$id" "${label:-$(sed -n '1s/^-- //p' "$f")}" "$on" "$off" "$(awk -v a="$on" -v b="$off" 'BEGIN {printf "%.1fx", b / a}')" "$plan" "$same"
done

log
log "$rows orders in $(( t2 - t0 )) s (data $(( t1 - t0 )) s, indexes $(( t2 - t1 )) s). Table, sift_col index, BM25 index: $sizes."
log "$(docker exec "$name" nproc) CPUs visible to the container${cpus:+ (quota $cpus)}, $(docker exec "$name" postgres --version). 'off' is the plan PostgreSQL has without SiftAgg on the same data and indexes."
log "Numbers depend on the host; the published ones are measured on a dedicated machine at 1,000,000 orders (bench/results/store.json)."
[[ -z $keep ]] || log "container $name is running: docker exec -it $name psql -U postgres -d store (docker rm -f -v $name when done)"
(( ! fail )) || { log "kit: an answer with the pushdown differs from the one without it"; exit 3; }
