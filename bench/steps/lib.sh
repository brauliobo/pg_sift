# Sourced by every bench script. Override any variable from the environment.
set -euo pipefail
export LC_ALL=C

STEPS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)   # the internal steps that bench/bench runs
BENCH=$(dirname "$STEPS")
registry() { awk -F'\t' -v col="$1" -v want="${2:-}" '!/^#/ && NF && $8 != "analytic" && (want == "" || $col == want) {printf "%s%s", sep, $1; sep = " "}' "$BENCH/engines.tsv"; }   # ids of the text engines whose column COL is WANT (any when empty), table order
ALL_ENGINES=$(registry 1)                    # every text engine of bench/engines.tsv (the analytic ones are the generic suites': steps/generic.py)
DEFAULT_ENGINES=$(registry 6 yes)            # what `bench run` measures without --engines
REF_ENGINES=$(awk -F'\t' '!/^#/ && NF && $7 != "-" {print $7}' "$BENCH/engines.tsv" | sort -u | paste -sd' ' -)   # the engines that are references for `bench regress`
PAIR_ENGINES=$(awk -F'\t' -v refs=" $REF_ENGINES " '!/^#/ && NF && $8 != "analytic" && ($7 != "-" || index(refs, " " $1 " ")) {printf "%s%s", sep, $1; sep = " "}' "$BENCH/engines.tsv")   # engines and references
SUITE_ENGINES=$(awk -F'\t' -v refs=" $REF_ENGINES " '!/^#/ && NF && $8 != "analytic" && !index(refs, " " $1 " ") {printf "%s%s", sep, $1; sep = " "}' "$BENCH/engines.tsv")   # every engine but the references
REPO=$(dirname "$BENCH")
. "$REPO/scripts/quiet.sh"                   # quiet_wait, quiet_batch, quiet_report: host noise by pressure stall information
. "$BENCH/versions.env"
. "$BENCH/presets.env"                       # the dataset ladder: smoke, quick, standard, full
command -v pg_config >/dev/null && PATH=$(pg_config --bindir):$PATH   # Debian keeps the server tools off PATH
ROOT=${BENCH_WORK:-${SIFT_BENCH_ROOT:-/tmp/sift-bench}}   # the work directory: clusters, scratch, engine builds (SIFT_BENCH_ROOT is the old name)
STORE=${BENCH_STORE:-$ROOT}                  # datasets, downloads, dumps and the cache of lanes: inputs and caches only, they can live on a disk while ROOT is tmpfs
TIER=${TIER:-Q}
declare -A TIER_PORT=([Q]=56040 [F]=56050)
declare -A TIER_DOCS=([Q]=${Q_DOCS:-150000} [F]=ALL)   # articles per tier; the cached dump carries the count
declare -A TIER_SHBUF=([Q]=1GB [F]=4GB)
declare -A TIER_MWM=([Q]=512MB [F]=2GB)
PGDATA=${PGDATA:-$ROOT/pg/$TIER}
SOCK=$ROOT/run
CACHE=$STORE/cache                           # corpus dumps and the cache of lanes (lanes.sh)
DUMP=$CACHE/sift-$TIER-${TIER_DOCS[$TIER]}.dump
DATA=$STORE/data                             # articles.tsv.gz, places-*/ (setup); Q/ and F/: exports of the corpus for the engines outside Postgres, derived
DOWNLOADS=${DOWNLOADS:-$STORE/download}      # the raw downloads, removed once the datasets are derived (KEEP_DOWNLOADS=1)
ENG=$ROOT/engines                            # engines/<name>/{dl,root}: downloads, and the installed (DESTDIR) or unpacked extension
SIFT_ROOT=${SIFT_ROOT:-$ENG/sift/root}
JOBS=${JOBS:-4}                              # parallel jobs of the builds in bench/steps/fetch.sh
OUT=${OUT:-$BENCH/out/$TIER}
LOCK=/tmp/sift-perf.lock
export PGHOST=$SOCK PGPORT=${PORT:-${TIER_PORT[$TIER]}} PGDATABASE=${DB:-bench} PGUSER=${PGUSER:-${USER:-$(id -un)}}

# Where the cluster looks for extensions (bench/steps/cluster.sh): every engine root first, then the system.
SHAREDIR=$(pg_config --sharedir 2>/dev/null || true) PKGLIBDIR=$(pg_config --pkglibdir 2>/dev/null || true)   # preflight (bench/bench) says what is missing
engine_paths() { { find "$ENG"/*/root -name "$1" -printf '%h\n' 2>/dev/null || true; } | sort -u; }   # directories holding files named $1
EXT_SHARE=$(engine_paths '*.control' | sed 's,/extension$,,' | paste -sd: -)
EXT_LIB=$(engine_paths '*.so' | paste -sd: -)

q()  { psql -X -v ON_ERROR_STOP=1 -qAt "$@"; }
log() { printf '[%s %s%s] %s\n' "$(date +%T)" "$TIER" "${LOG_TAG:+ $LOG_TAG}" "$*" >&2; }
loadavg() { cut -d' ' -f1 /proc/loadavg; }   # only the `load` column of the result tables, kept for the older runs; noise is judged by quiet_report
# The noise of a timed batch is judged by what others did in its window, not by waiting for a quiet host: the pressure stall information cannot tell the
# run's own servers and clients from other users, and waiting for it idled the run. A window is the time from one `batch` call to the next: the CPU the host
# spent (/proc/stat) less what the cgroup of this run used (the scope it was started in: servers, clients, the Elasticsearch process) is the work of somebody
# else; a window is NOISY when it is above SIFT_NOISE_CPU (0.25) cores over the idle host's, measured once, 10 s, at the first batch. Recorded in noisy.txt.
# server_run CMD...: CMD as a server of the run. With SERVER_BUDGET=N (CPUs) it is a systemd scope of the slice sift-bench with CPUQuota N00% on the cores
# SERVER_CPUS, so that the servers have the same budget while the clients (the harness, pgbench, the load generator) run outside the cap on other cores
server_run() {
  if [[ -n ${SERVER_BUDGET:-} ]]; then
    systemd-run --user --scope -q --slice=sift-bench -p CPUQuota=$(( SERVER_BUDGET * 100 ))% -p MemoryMax=12G -p MemorySwapMax=0 -p OOMPolicy=continue taskset -c "${SERVER_CPUS:?SERVER_CPUS}" "$@"
  else "$@"; fi
}
sentinel_now() { # host busy seconds, CPU seconds of this run (the harness' cgroup and the servers' slice), uptime seconds
  awk -v hz="$(getconf CLK_TCK)" '/^cpu / {printf "%.3f ", ($2 + $3 + $4 + $7 + $8 + $9) / hz}' /proc/stat
  local own srv
  own=$(awk '/^usage_usec/ {print $2 / 1e6}' "/sys/fs/cgroup$(cut -d: -f3 /proc/self/cgroup)/cpu.stat" 2>/dev/null || echo 0)
  srv=$(awk '/^usage_usec/ {print $2 / 1e6}' "/sys/fs/cgroup$(systemctl --user show -p ControlGroup --value sift-bench.slice 2>/dev/null)/cpu.stat" 2>/dev/null || echo 0)
  awk -v a="${own:-0}" -v b="${srv:-0}" 'BEGIN {printf "%.3f ", a + b}'
  cut -d" " -f1 /proc/uptime
}
sentinel_rate() { awk -v a="$1" -v b="$2" 'BEGIN {split(a, x, " "); split(b, y, " "); dt = y[3] - x[3]; printf "%.3f", (dt > 0) ? ((y[1] - x[1]) - (y[2] - x[2])) / dt : 0}'; }
batch_close() { # judge the window that is open (the one since the last batch call)
  [[ -n ${SENT_AT:-} ]] || return 0
  local now rate; now=$(sentinel_now); rate=$(sentinel_rate "$SENT_AT" "$now")
  if awk -v r="$rate" -v b="$SENT_BASE" -v m="${SIFT_NOISE_CPU:-0.25}" 'BEGIN {exit !(r - b > m)}'; then
    log "$SENT_WHAT: NOISY, $rate cores of other work over the $SENT_BASE of the idle host ($(quiet_report 2>&1), for information)"
    printf '%s\t%s cores of other work over the %s of the idle host\n' "$SENT_WHAT" "$rate" "$SENT_BASE" >>"${DEST:-$OUT}/noisy.txt"
  fi
  SENT_AT=
}
batch() { # WHAT: opens the window of a timed batch (and judges the one before it)
  batch_close
  if [[ -z ${SENT_BASE:-} ]]; then
    local a b; a=$(sentinel_now); sleep 10; b=$(sentinel_now); SENT_BASE=$(sentinel_rate "$a" "$b")
    log "the idle host has $SENT_BASE cores of other work (a batch is NOISY above ${SIFT_NOISE_CPU:-0.25} over it)"
  fi
  SENT_WHAT=$*; SENT_AT=$(sentinel_now)
}
noisy_report() { # the pressure now and the number of batches that ran NOISY
  batch_close
  local n=0; [[ ! -f ${DEST:-$OUT}/noisy.txt ]] || n=$(wc -l <"${DEST:-$OUT}/noisy.txt")
  echo "$(quiet_report 2>&1), $n NOISY batches"
}
# Timed sections must not overlap with other jobs' timings. `bench` holds the lock for a whole run (PERF_LOCK_HELD).
locked() { if [[ -n ${PERF_LOCK_HELD:-} ]]; then "$@"; else ( flock 9; "$@" ) 9>>"$LOCK"; fi; }
now_ms() { date +%s%3N; }
elapsed_s() { awk -v a="$1" -v b="$2" 'BEGIN {printf "%.1f", (b - a) / 1000}'; }   # ms timestamps -> seconds
engine_db() { echo "e_${1//-/_}"; }
db_exists() { [[ $(PGDATABASE=postgres q -c "SELECT count(*) FROM pg_database WHERE datname = '$1'") == 1 ]]; }
drop_db() { PGDATABASE=postgres q -c "DROP DATABASE IF EXISTS $1 WITH (FORCE)"; }

# An adapter (bench/engines/<name>.sh) describes one engine for every dataset. Articles (the `docs` table): REQUIRES
# (extension), SETUP and INDEX (SQL), and either PRED and RANK (SQL expressions over `mode` and `terms` that render the
# WHERE condition and the ORDER BY of a query, NULL when the engine cannot answer it; SOURCE: the FROM item of ranked
# statements when it is not `docs WHERE PRED`; HELPERS: SQL they use; PROBE: see engine_available) or, for engines
# outside Postgres, EXTERNAL, RENDER (+ JOINS when the engine answers the join class) and the x_* functions (bench/steps/xlib.sh). Places (the
# `places` table, steps/places.sh): PLACES_SETUP, PLACES_INDEX (+ PLACES_INDEXES: the relations whose sizes add up), the optional emulation
# of workloads the engine has no native query for (PLACES_SETUP2, PLACES_INDEX2, PLACES_INDEXES2) and the statements P_match, P_top,
# P_boost, P_filter, P_count, P_scan (empty: no such query); an external engine has PLACES_RENDER and x_build_places instead. A variant
# adapter sets V_EXT / V_OPTS, then sources its base.
# shellcheck disable=SC2016  # $b$ is the SQL quote of the function body
render_sql() {
  echo "${HELPERS:-}"
  printf 'CREATE OR REPLACE FUNCTION bench_pred(mode text, terms text[]) RETURNS text LANGUAGE sql AS $b$ SELECT %s $b$;\n' "$PRED"
  printf 'CREATE OR REPLACE FUNCTION bench_rank(mode text, terms text[]) RETURNS text LANGUAGE sql AS $b$ SELECT %s $b$;\n' "$RANK"
  printf 'CREATE OR REPLACE FUNCTION bench_source(mode text, terms text[]) RETURNS text LANGUAGE sql AS $b$ SELECT %s $b$;\n' "${SOURCE:-NULL::text}"
}
XBENCH=$ROOT/target/release/xbench          # the Rust load generator of the external engines (bench/steps/fetch.sh tantivy)
load_engine() {
  ENGINE=$1; REQUIRES=''; PROBE=''; SETUP=''; INDEX=''; PRED=''; RANK=''; SOURCE=''; HELPERS=''; RENDER=''; EXTERNAL=''; REQUIRES_PATH=''; JOINS=''
  PLACES_SETUP=''; PLACES_INDEX=''; PLACES_INDEXES=''; PLACES_SETUP2=''; PLACES_INDEX2=''; PLACES_INDEXES2=''; PLACES_FLAVOR=''; PLACES_FLAVOR_MATCH=''; PLACES_RENDER=''
  P_match=''; P_top=''; P_boost=''; P_filter=''; P_count=''; P_scan=''; V_EXT=''; V_OPTS=''
  . "$BENCH/engines/_places.sh"
  . "$BENCH/engines/$1.sh"
  [[ -n $RENDER || -n $EXTERNAL ]] || RENDER=$(render_sql)
}
# does the loaded engine run the dataset (articles, places)?
supports() { case $1 in articles) [[ -n $INDEX || -n $EXTERNAL ]] ;; places) [[ -n $PLACES_INDEX || -n $PLACES_RENDER ]] ;; esac; }
# PROBE (adapters): a statement over a temp table `probe (t text)` that only works when the installed extension has what the
# engine needs (a reloption of a newer build); it runs in a rolled back transaction. External engines need their binary.
engine_available() {
  [[ -z $EXTERNAL ]] || { [[ -x $REQUIRES_PATH ]]; return; }
  [[ -z $REQUIRES ]] || [[ $(PGDATABASE=postgres q -c "SELECT count(*) FROM pg_available_extensions WHERE name = '$REQUIRES'") == 1 ]] || return 1
  [[ -z $PROBE ]] || PGDATABASE=postgres q -c "BEGIN; CREATE EXTENSION IF NOT EXISTS $REQUIRES; CREATE TEMP TABLE probe (t text); $PROBE; ROLLBACK" >/dev/null 2>&1
}
# Prints the engines of $1 that run dataset $2 and are installed; says why the others are left out (stderr).
available_engines() {
  local e
  for e in $1; do
    if ! ( load_engine "$e"; supports "$2" ); then log "skipping $e: it has no $2 workload" >&2
    elif ( load_engine "$e"; engine_available ); then echo "$e"
    else log "skipping $e: not installed or lacks what it needs (bench/bench setup --engines $e)" >&2; fi
  done
}
clone_db() {
  PGDATABASE=postgres q -c "DROP DATABASE IF EXISTS $1 WITH (FORCE)" -c "CREATE DATABASE $1 TEMPLATE bench STRATEGY FILE_COPY"
  export DB=$1 PGDATABASE=$1
  q -f "$BENCH/sql/harness.sql"
}
adopt_db() { # NAME: in a cluster of one engine the corpus database `bench` becomes the engine's database (no second copy), with the harness
  PGDATABASE=postgres q -c "ALTER DATABASE bench RENAME TO $1"
  export DB=$1 PGDATABASE=$1
  q -f "$BENCH/sql/harness.sql"
}
# Untimed engine database for correctness checks, reused across calls unless REBUILD=1.
check_db() {
  local db=o_$ENGINE
  if [[ -z ${REBUILD:-} ]] && db_exists "$db"; then
    export DB=$db PGDATABASE=$db
  else
    clone_db "$db"
    q -c "$SETUP" -c "$INDEX" -c "VACUUM (ANALYZE) docs" 2>/dev/null
  fi
  q -c "$RENDER"
}

# --- interleaved rounds (bench/steps/articles.sh, places.sh)
# rounds FN: runs FN ROUND for MAXR rounds, stopping after MINR once the caller's `stable` holds.
rounds() {
  local r
  (( MAXR <= 3 )) || log "warning: MAXR=$MAXR: more than 3 rounds (the most that a run takes by default), the run takes that much longer"
  for r in $(seq "$MAXR"); do
    if [[ -e $DEST/round-$r.done ]]; then log "round $r: measured before, kept"
    else
      "$1" "$r"
      echo "$ROUND_KEY" | atomic_write "$DEST/round-$r.done"
      log "round $r done, $(quiet_report 2>&1)"
    fi
    (( r >= MINR )) && stable && break
  done
  return 0
}
# cells_begin HEADER: DEST/cells.tsv with its header line. A run that is started again with the same code, host and settings (bench/bench resume) keeps the rows of the rounds that finished
# (a round is done when DEST/round-N.done holds the key; the rows of a round that was cut off are dropped); any other key starts the table over
cells_begin() {
  local last=0 f
  ROUND_KEY=$(python3 -I "$STEPS/benchlib.py" key "$(code_key)" "$(fingerprint)" "$ENGINES" "${CLASSES:-}" "${WLS:-}" "$RSE ${SECS:-} ${MIN_TIME:-} ${MIN_N:-} ${MINR:-} ${MAXR:-} ${CLIENTS:-} $TIER ${Q_DOCS:-}")
  if [[ -z ${NO_RESUME:-} && -s $DEST/cells.tsv ]]; then
    for f in "$DEST"/round-*.done; do [[ -e $f && $(cat "$f") == "$ROUND_KEY" ]] && last=$(basename "$f" .done) && last=${last#round-}; done
  fi
  if (( last )); then
    awk -F'\t' -v m="$last" 'NR == 1 || $1 <= m' "$DEST/cells.tsv" | atomic_write "$DEST/cells.tsv"
    for f in "$DEST"/round-*.done; do [[ ! -e $f || $(basename "$f" .done) == round-* && $(cat "$f") == "$ROUND_KEY" ]] || rm -f "$f"; done
    log "resume: the cells of rounds 1-$last are kept"
  else
    rm -f "$DEST"/round-*.done
    printf '%s\n' "$1" | atomic_write "$DEST/cells.tsv"
  fi
  export ROUND_KEY
}
rotated() { local r=$1 n=$(( $# - 1 )) k; shift; local e=("$@"); for ((k = 0; k < n; k++)); do echo "${e[$(( (k + r) % n ))]}"; done; }   # engines in the order of round r
# rse_cells FILE KEYCOLS VALCOL [SECOL]: the cells (their KEYCOLS joined by a tab, e.g. 3,4 = class and engine, one per line) of the
# tab separated FILE (header first) that need another round: the relative standard error of the numbers of VALCOL over the rounds
# so far is above RSE (default 5%). The error is the larger of sd / sqrt(n) / mean over the rounds and, with SECOL (the standard
# error of each round's value, cell.py), the error that the rounds' own errors leave: sqrt(sum se^2) / n / mean. A cell whose
# rounds are so uncertain on their own that MAXR rounds cannot bring it to RSE (a slow cell limited by time) is not repeated:
# another round would cost as much as the first and move the error by a factor of sqrt(n / (n + 1)). NA cells are ignored, and so
# are cells with a failed run (0, nan): the failure is the result, another round would only wait for the timeout again.
rse_cells() {
  awk -F'\t' -v rse="${RSE:-0.05}" -v maxr="${MAXR:-3}" -v kc="$2" -v vc="$3" -v sc="${4:-0}" 'BEGIN { nk = split(kc, K, ",") }
    NR > 1 && $vc != "NA" { k = $(K[1]); for (i = 2; i <= nk; i++) k = k "\t" $(K[i])
      if (!(k in n)) order[++m] = k
      n[k]++; if ($vc !~ /^[0-9.]+$/ || $vc == 0) bad[k] = 1; else { s[k] += $vc; q[k] += $vc * $vc
        if (sc && $sc ~ /^[0-9.]+$/) { se2[k] += $sc * $sc; rel2[k] += ($sc / $vc) ^ 2; known[k]++ } } }
    END { for (j = 1; j <= m; j++) { k = order[j]; if (bad[k]) continue
          mean = s[k] / n[k]; var = n[k] > 1 ? (q[k] - n[k] * mean * mean) / (n[k] - 1) : 0; if (var < 0) var = 0
          if (known[k] < n[k]) { if (n[k] < 2 || (mean > 0 && sqrt(var / n[k]) / mean > rse)) print k; continue }
          between = sqrt(var / n[k]) / mean; within = sqrt(se2[k]) / n[k] / mean
          if ((between > rse || within > rse) && sqrt(rel2[k] / n[k] / maxr) <= rse) print k } }' "$1"
}

# --- cold cache: empty the relations of a table from shared buffers and the OS page cache, then time one statement
# cold_table TABLE: the table the cold statements read; its heap, TOAST table and indexes are evicted before each one. A data
# directory in memory (tmpfs) has no page cache apart from the shared buffers (a file there is the page cache): nothing else to evict.
cold_table() {
  COLD_RELS="SELECT oid FROM pg_class WHERE oid = '$1'::regclass OR oid = (SELECT reltoastrelid FROM pg_class WHERE oid = '$1'::regclass)
             UNION SELECT indexrelid FROM pg_index WHERE indrelid = '$1'::regclass"
  COLD_FILES=''
  [[ $(stat -f -c %T "$PGDATA") == tmpfs ]] || COLD_FILES=$(q -c "SELECT pg_relation_filepath(c) FROM ($COLD_RELS) x(c)")
}
cold_ms() { # STATEMENT [psql args]: execution time in ms of the statement (EXPLAIN ANALYZE) right after the eviction of cold_table's relations
  local explain="EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON, COSTS OFF) $1" evict="SELECT pg_buffercache_evict_relation(c) FROM ($COLD_RELS) x(c)"
  if [[ -z $COLD_FILES ]]; then
    printf '%s;\n%s;\n' "$evict" "$explain" | psql -X -qAt "${@:2}" | sed -n 's/^Execution Time: \([0-9.]*\) ms/\1/p'   # on stdin: psql variables are not interpolated in -c
    return
  fi
  q -c "$evict" >/dev/null
  # shellcheck disable=SC2086  # one argument per relation file
  python3 -I - "$PGDATA" $COLD_FILES <<'PY'
import glob, os, sys
for rel in sys.argv[2:]:
    for f in glob.glob(os.path.join(sys.argv[1], rel) + '*'):
        fd = os.open(f, os.O_RDONLY); os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED); os.close(fd)
PY
  echo "$explain" | psql -X -qAt "${@:2}" | sed -n 's/^Execution Time: \([0-9.]*\) ms/\1/p'
}
# cold_cell FN: p50, maximum and the number of the times (ms, tab separated) of `FN i`, i = 1..NQ: at least COLD_MIN of them (default 5),
# then only while the cell has used less than COLD_TIME seconds (default 3): a statement of seconds is not run twelve times
cold_cell() {
  local i t0; t0=$(now_ms)
  for (( i = 1; i <= NQ; i++ )); do
    "$1" "$i"
    (( i < ${COLD_MIN:-5} || $(now_ms) - t0 < ${COLD_TIME:-3} * 1000 )) || break
  done | median_max
}
median_max() { sort -n | awk '{v[NR] = $1} END {if (NR) printf "%.2f\t%.2f\t%d", v[int((NR + 1) / 2)], v[NR], NR; else printf "error\terror\t0"}'; }   # p50, maximum and count of the numbers on stdin (error: none)

# --- shared by every step and by bench/bench: this is the one place for them, a step does not define them again
die() { echo "${DIE_TAG:-bench}: $*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }
need() { local c; for c in "$@"; do have "$c" || die "missing prerequisite: $c"; done; }
scratch() { mktemp -d "${SIFT_TMP:-/tmp}/sift-$1.XXXXXX"; }   # NAME: a new scratch directory; the caller removes it (trap)
port_free() { [[ -z $(ss -ltnH "sport = :$1") ]]; }          # PORT
ports_free() { local p; for p in $(seq "$1" $(( $1 + $2 - 1 ))); do port_free "$p" || { echo "$(basename "$0"): port $p is in use" >&2; return 1; }; done; }   # BASE COUNT
perf_hold() { # one run at a time on the machine: the run holds the perf lock until it ends (the timed steps of a run that does not hold it take it themselves: `locked`)
  [[ -z ${PERF_LOCK_HELD:-} ]] || return 0
  exec 8>>"$LOCK"
  flock -n 8 || { echo "waiting for the perf lock (another benchmark is timing)"; flock 8; }
  export PERF_LOCK_HELD=1
}
pg_container() { # NAME PORT IMAGE [SERVER SETTINGS...]: a PostgreSQL container on 127.0.0.1:PORT (trust authentication, the caps of CAPS, a PG_LABEL if set), started again if it exists, ready when it returns
  local n=$1 p=$2 i=$3 t; shift 3
  docker rm -fv "$n" >/dev/null 2>&1 || true
  docker run -d --name "$n" ${PG_LABEL:+--label "$PG_LABEL"} "${CAPS[@]}" --shm-size 1g -p "127.0.0.1:$p:5432" -e POSTGRES_HOST_AUTH_METHOD=trust "$i" postgres "$@" >/dev/null
  for (( t = 0; t < 300; t++ )); do pg_isready -q -h 127.0.0.1 -p "$p" && return 0; sleep 1; done
  die "the server on port $p did not start (docker logs $n)"
}
# capped CMD...: the standing caps of the work on a PostgreSQL that is not in a container: a memory cap with no swap (a runaway query dies alone), nice, and a CPU quota when CAP_CPU is set
capped() { systemd-run --user --scope -q -p MemoryMax="${CAP_MEM:-${GAP_MEM:-12G}}" -p MemorySwapMax=0 -p OOMPolicy=continue ${CAP_CPU:+-p CPUQuota="$CAP_CPU"} nice "$@"; }
docker_caps() { printf '%s\n' --cpus "$1" --memory "$2" --memory-swap "$2"; }   # CPUS MEMORY: the quota (never a pinning) and the memory limit without swap of every container
# The same flags for all the PostgreSQL containers of the store and duckdb benchmarks. The parallel pool (max_worker_processes, max_parallel_workers) is far above the budget and the cost
# knobs are low, so that nothing but max_parallel_workers_per_gather (N - 1) decides how many processes a scan uses.
PG_FLAGS=(-c shared_buffers=2GB -c effective_cache_size=6GB -c work_mem=64MB -c maintenance_work_mem=512MB -c max_wal_size=8GB -c jit=off
          -c max_worker_processes=16 -c max_parallel_workers=16 -c parallel_setup_cost=100 -c parallel_tuple_cost=0.01
          -c min_parallel_table_scan_size=1MB -c min_parallel_index_scan_size=64kB)
pull_missing() { docker image inspect "$1" >/dev/null 2>&1 || docker pull -q "$1" >/dev/null; }
pg_mount() { # IMAGE: the directory the image declares for its data (the official image: /var/lib/postgresql, PGDATA below it)
  local d; d=$(docker image inspect -f '{{range $k, $_ := .Config.Volumes}}{{println $k}}{{end}}' "$1" | head -1)
  [[ -n $d ]] || die "the image $1 declares no volume for its data"
  echo "$d"
}
# sift_image TAG BASE: the image TAG, the official PostgreSQL image BASE plus pg_sift built from the tracked files of this checkout (as scripts/replication.sh); built again only when they changed
sift_image() {
  local ctx key
  ctx=$(scratch store-image)
  (cd "$REPO" && git ls-files -z Makefile pg_sift.control 'sql/pg_sift--*.sql' src scripts/harden.mk | tar --null -T - -cf -) | tar -xf - -C "$ctx"
  key=$(tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner -cf - -C "$ctx" . | sha256sum | cut -c1-12)
  if [[ $(docker image inspect -f '{{index .Config.Labels "sift-store-key"}}' "$1" 2>/dev/null) == "$key" ]]; then log "image $1 is up to date"; rm -rf "$ctx"; return; fi
  pull_missing "$2"
  cat >"$ctx/Dockerfile" <<EOF
FROM $2
LABEL sift-store-key=$key
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends build-essential liblz4-dev libzstd-dev libicu-dev postgresql-server-dev-\$PG_MAJOR
COPY . /src
RUN nice make -C /src -j${IMAGE_JOBS:-4} USE_PGXS=1 install && rm -rf /src
EOF
  log "building $1 from $2"
  nice docker build -q -t "$1" "$ctx" >/dev/null
  rm -rf "$ctx"
}
fingerprint() { python3 -I "$STEPS/benchlib.py" fingerprint --id; }              # the host: CPU, threads, memory, kernel, docker, governor (steps/benchlib.py)
code_key() { # what a result depends on besides the host: the pg_sift sources under test and the harness (scripts, SQL, adapters, pins; not the documentation)
  { git -C "$REPO" ls-files -z Makefile pg_sift.control 'sql/pg_sift--*.sql' src scripts/harden.mk | (cd "$REPO" && xargs -0 sha256sum)
    git -C "$REPO" ls-files -z --cached --others --exclude-standard -- bench ':(exclude)*.md' ':(exclude)bench/out' ':(exclude)*__pycache__*' | (cd "$REPO" && xargs -0 sha256sum); } | sha256sum | cut -c1-16
}

phase() { printf '%s\t%s\n' "$1" "$(( SECONDS - T0 ))" >>"$DEST/phases.txt"; T0=$SECONDS; }   # NAME: seconds since the last phase, to $DEST/phases.txt (articles, places)

download() { # URL FILE: skipped when FILE is there; a partial download (FILE.part) is continued with a range request and FILE appears only when it is complete
  [[ -s $2 ]] && return 0
  curl -fsSL -C - -m "${DL_TIMEOUT:-3600}" -o "$2.part" "$1" || { rm -f "$2.part"; curl -fsSL -m "${DL_TIMEOUT:-3600}" -o "$2.part" "$1"; }
  mv -f "$2.part" "$2"
}

# --- runs that can be resumed (bench/bench resume, final, steps/store.sh)
# A run has an id (bench/out/run-ID) and a manifest in $OUT/.run: argv (the command line to run again), pid, state (running | done | failed | interrupted | crashed) and, per stage, the state
# file stages/NAME.state `state<TAB>key<TAB>date<TAB>note`. The key is the code, host fingerprint and settings the stage's results depend on: a stage that is `done` with the same key is skipped,
# anything else runs again, results of another key are never reused. Every state and result file is written whole or not at all (atomic_write). SIGINT and SIGTERM stop the stage's processes,
# record `interrupted` and exit with 75 (EX_TEMPFAIL, resumable); a failed stage is recorded `failed` and exit status is its own; a SIGKILL or a crash leaves `running`, which the next start of
# the run finds out by the pid (run_reclaim). Locks are flock locks: a dead process holds none.
EX_RESUMABLE=75
atomic_write() { local f=$1 t; t=$(mktemp "$f.XXXXXX"); cat >"$t"; sync "$t"; mv -f "$t" "$f"; }   # FILE: stdin becomes FILE, whole or not at all
run_dir() { echo "${RUN_DIR:-$OUT/.run}"; }
run_state() { printf '%s\t%s\t%s\n' "$1" "$(date -u +%FT%TZ)" "${2:-}" | atomic_write "$(run_dir)/state"; }   # STATE [NOTE]
run_begin() { # ARGV...: the manifest of this run; the command line is what `bench resume` runs again
  mkdir -p "$(run_dir)/stages"
  STAGE_STATE=$(run_dir)/stages
  printf '%q ' "$@" | atomic_write "$(run_dir)/argv"
  echo $$ | atomic_write "$(run_dir)/pid"
  fingerprint | atomic_write "$(run_dir)/fingerprint"
  run_state running
  trap 'run_exit $?' EXIT
  trap 'run_signal INT' INT
  trap 'run_signal TERM' TERM
}
kill_tree() { local p; for p in $(pgrep -P "$1" 2>/dev/null || true); do kill_tree "$p"; done; kill -TERM "$1" 2>/dev/null || true; }   # PID and everything below it
run_signal() { # the stage's processes stop, the state is recorded, the exit status says resumable
  trap '' INT TERM
  [[ -z ${STAGE_PID:-} ]] || { kill_tree "$STAGE_PID"; wait "$STAGE_PID" 2>/dev/null || true; }
  log "$1: stopped; bench/bench resume continues from the last finished stage"
  exit "$EX_RESUMABLE"
}
run_exit() { # EXIT: the state of the run and of the stage that was running
  local rc=$1
  trap '' INT TERM
  [[ -z ${PEAK_PID:-} ]] || kill "$PEAK_PID" 2>/dev/null || true   # the sampler of the footprint (steps/footprint.sh)
  [[ -z ${CURRENT_STAGE:-} ]] || stage_state "$CURRENT_STAGE" "$( (( rc == EX_RESUMABLE )) && echo interrupted || echo failed )" "$CURRENT_KEY" "exit $rc"
  [[ ! -d $(run_dir) ]] || run_state "$( (( rc == 0 )) && echo 'done' || { (( rc == EX_RESUMABLE )) && echo interrupted || echo failed; } )" "exit $rc"
}
ppid() { sed -E 's/^.*\) [A-Za-z] ([0-9]+) .*/\1/' "/proc/$1/stat" 2>/dev/null; }   # PID: its parent
run_procs() { # RUN-ID: the processes that carry the run's id in their environment (BENCH_RUN_ID) and are not this shell or below it: the run's own, orphans of a killed attempt included
  local e p q
  for e in /proc/[0-9]*/environ; do
    p=${e#/proc/}; p=${p%/environ}
    grep -qaxz "BENCH_RUN_ID=$1" "$e" 2>/dev/null || continue
    for (( q = p; q > 1; q = $(ppid "$q") )); do [[ $q == "$$" ]] && continue 2; done
    echo "$p"
  done
  return 0
}
run_stop() { # RUN-ID: the run's processes (not this one) stop: TERM, then KILL after 3 s
  local p left
  for p in $(run_procs "$1"); do kill -TERM "$p" 2>/dev/null || true; done
  for _ in 1 2 3 4 5 6; do left=$(run_procs "$1"); [[ -z $left ]] && return 0; sleep 0.5; done
  for p in $left; do kill -KILL "$p" 2>/dev/null || true; done
}
run_alive() { # RUN-DIR: the process of a run that says it is running is there (a bench, by its command line)
  local pid; [[ -s $1/pid ]] || return 1
  pid=$(<"$1/pid"); [[ -d /proc/$pid && $(tr '\0' ' ' <"/proc/$pid/cmdline" 2>/dev/null) == *bench* ]]
}
run_reclaim() { # at the start: a run whose process is gone but whose state says running crashed (killed, rebooted): what it left running stops and its containers go, its finished stages stay for `resume`
  local d id
  for d in "${RUNS_DIR:-$BENCH/out}"/run-*/.run; do
    [[ -s $d/pid && $(cut -f1 "$d/state" 2>/dev/null) == running ]] || continue
    run_alive "$d" && continue
    id=$(basename "$(dirname "$d")"); id=${id#run-}
    log "run $id: its process $(<"$d/pid") is gone (state running): crashed; its processes and containers are removed, \`bench/bench resume $id\` continues it"
    printf 'crashed\t%s\t\n' "$(date -u +%FT%TZ)" | atomic_write "$d/state"
    run_stop "$id"
    run_containers "$id" | xargs -r docker rm -fv >/dev/null
  done
}
run_containers() { docker ps -aq --filter "label=sift-run=$1" 2>/dev/null || true; }   # RUN-ID: the containers a run started (they carry the label)

# --- stages with a time budget
# stage NAME MINUTES CMD...: runs CMD (a child of this shell, so that a signal can stop it) and logs the time against its budget; over 1.3x of it is logged OVER BUDGET (BUDGET_STRICT=1: and
# stops). STAGE_LOG: a file the stage appends `name<TAB>seconds<TAB>budget minutes` to
stage() {
  local name=$1 budget=$2 t0=$SECONDS took
  shift 2
  "$@" &
  STAGE_PID=$!
  wait "$STAGE_PID"
  STAGE_PID=''
  took=$(( (SECONDS - t0 + 30) / 60 ))
  [[ -z ${STAGE_LOG:-} ]] || printf '%s\t%s\t%s\n' "$name" "$(( SECONDS - t0 ))" "$budget" >>"$STAGE_LOG"
  log "stage $name: $took min (budget $budget)"
  if (( budget > 0 && (SECONDS - t0) * 10 > budget * 60 * 13 )); then
    log "OVER BUDGET: stage $name took $took min, budget $budget (1.3x = $((budget * 13 / 10)))"
    [[ -z ${BUDGET_STRICT:-} ]] || exit 4
  fi
}
stage_state() { printf '%s\t%s\t%s\t%s\n' "$2" "$3" "$(date -u +%FT%TZ)" "${4:-}" | atomic_write "$STAGE_STATE/$1.state"; }   # NAME STATE KEY [NOTE]
stage_done() { # NAME KEY: the stage finished before with this key (NO_RESUME=1: never)
  local st k
  [[ -z ${NO_RESUME:-} && -s $STAGE_STATE/$1.state ]] || return 1
  IFS=$'\t' read -r st k _ <"$STAGE_STATE/$1.state"
  [[ $st == 'done' && $k == "$2" ]]
}
# stage_run NAME KEY MINUTES CMD...: `stage`, unless the stage is done with KEY; its state is recorded before, and after (done), a failure or an interruption (run_exit)
stage_run() {
  local name=$1 key=$2
  shift 2
  if stage_done "$name" "$key"; then
    log "stage $name: finished before ($(cut -f3 "$STAGE_STATE/$name.state")), resumed"
    return 0
  fi
  CURRENT_STAGE=$name CURRENT_KEY=$key
  stage_state "$name" running "$key" "pid $$"
  stage "$name" "$@"
  stage_state "$name" 'done' "$key"
  CURRENT_STAGE=''
}
