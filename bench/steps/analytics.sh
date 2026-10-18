#!/usr/bin/env bash
# Usage: bench/bench analytics setup|run|report|clean   (bench/steps/analytics.sh, called by bench/bench: it holds the perf lock)
#        bench/bench analytics clickbench              (the ClickBench subset: explicit, bench/steps/analytics.sh clickbench)
# Analytics benchmark of sift_col: an orders corpus (scalar columns, jsonb objects and arrays of objects) in a PostgreSQL schema of the usual e-commerce ORM shape, the
# report queries on it with the baseline plan and the sift_col plan, optionally on flat tables and on a throwaway ClickHouse.
#   setup  build sift (this checkout), start a cluster of its own, generate and load the corpus, build the flat tables / the
#          ClickHouse tables / the sift_col index when asked and available. What is loaded and unchanged is not loaded again.
#   run    setup, then steps/analytics.py measures into the result directory; the servers stay up until `clean`
# Everything lives under $SIFT_TMP (default $BENCH_WORK/analytics): the cluster on SIFT_PORT (default 56060), ClickHouse on SIFT_PORT + 10
# (native) and + 11 (http); only these are ever connected to. Settings come from bench/bench as A_* variables.
set -euo pipefail
export BENCH_WORK=${SIFT_TMP:-${BENCH_WORK:-${SIFT_BENCH_ROOT:-/tmp/sift-bench}}/analytics}
export PORT=${SIFT_PORT:-56060} DB=${A_DB:-analytics} PGDATA=$BENCH_WORK/pg SHBUF=${A_SHBUF:-4GB}
. "$(dirname "$0")/lib.sh"

SQLD=$BENCH/sql/analytics
A_SCALE=${A_SCALE:-1}
A_ORDERS=${A_ORDERS:-$(( 1200000 * A_SCALE ))}
A_STORES=${A_STORES:-40000}
A_CLUSTER=${A_CLUSTER:-}                     # store: the heap is written clustered by store and date (default: insertion order)
A_FLAT=${A_FLAT:-}                           # 1: the flat tables pg_items / pg_metrics / pg_notes
A_CH=${A_CH:-}                               # 1: the ClickHouse comparison
A_RUNS=${A_RUNS:-5}
A_BUILD=${A_BUILD:-1}                        # 0: do not build sift from this checkout (the sift_col access method lands with it)
A_COL_INDEX=${A_COL_INDEX:-}                 # the sift_col DDL: a statement, or @FILE (default $SQLD/col_index.sql)
A_COL_EXT=${A_COL_EXT:-pg_sift}              # the extension that provides the access method
A_COL_AM=${A_COL_AM:-sift_col}
A_ROWS=${A_ROWS:-10000000}                   # clickbench: rows of hits
CH_BIN=${CH_BIN:-/usr/bin/clickhouse}
CH_DIR=$ROOT/ch CH_TCP=$(( PORT + 10 )) CH_HTTP=$(( PORT + 11 ))
CB_DB=clickbench
CB_COMMIT=${CB_COMMIT:-fb42b2d7fb47d29ad9ca976c14220e6e63e2b889}   # ClickHouse/ClickBench, directory postgresql/
CB_DIR=$STORE/clickbench                     # queries.sql, create.sql (pinned), parquet downloads while loading
CB_SHA256_queries=${CB_SHA256_queries:-a7d6673357348ee9680443216b6f26f30d1dce9f313b419d38502417b2c2a219}
CB_SHA256_create=${CB_SHA256_create:-42d28575fd59fb4ae5d556683323c17937daaae8ff800e9089ddaaf91d1b2be7}
CB_BASE=https://datasets.clickhouse.com/hits_compatible/athena_partitioned

meta() { q -c "INSERT INTO analytics_meta VALUES ('$1', '$2') ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value"; }
meta_get() { q -c "SELECT value FROM analytics_meta WHERE key = '$1'"; }
ch_client() { "$CH_BIN" client --host 127.0.0.1 --port "$CH_TCP" "$@"; }

# --- the cluster
cluster_start() {
  mkdir -p "$ROOT"
  TIER=Q "$STEPS/cluster.sh" init
  # no WAL for the load of a table created in the same transaction, little WAL kept (the data directory is RAM)
  printf 'wal_level = minimal\nmax_wal_senders = 0\nmax_wal_size = 192MB\nmin_wal_size = 32MB\n' >"$PGDATA/analytics.conf"
  grep -q analytics.conf "$PGDATA/postgresql.conf" || echo "include_if_exists = 'analytics.conf'" >>"$PGDATA/postgresql.conf"
  TIER=Q "$STEPS/cluster.sh" start
}
create_db() { db_exists "$1" || PGDATABASE=postgres q -c "CREATE DATABASE $1"; }

build_sift() {
  [[ $A_BUILD == 0 ]] || "$STEPS/fetch.sh" sift
}

# --- the corpus
stamp() { echo "$A_ORDERS $A_STORES ${A_CLUSTER:-id} $(cat "$SQLD/schema.sql" "$SQLD/gen.sql" "$SQLD/sets.sql" "$SQLD/indexes.sql" | sha256sum | cut -c1-12)"; }

load() {
  local t0 order=''
  if db_exists "$DB" && [[ $(meta_get stamp) == "$(stamp)" ]]; then log "$DB: $A_ORDERS orders already loaded"; return; fi
  [[ $A_CLUSTER != store ]] || order='ORDER BY p.tenant_id, p.placed_at, g.id'
  drop_db "$DB"; create_db "$DB"
  t0=$(now_ms)
  log "generating $A_ORDERS orders in $A_STORES tenants${A_CLUSTER:+, clustered by $A_CLUSTER}"
  q -1 -v n="$A_ORDERS" -v tenants="$A_STORES" -v "order_by=$order" -f "$SQLD/schema.sql" -f "$SQLD/gen.sql"
  q -f "$SQLD/indexes.sql" -f "$SQLD/sets.sql"
  q -c "VACUUM (ANALYZE) orders" -c "VACUUM (ANALYZE) metrics_map" -c "VACUUM (ANALYZE) categories_map" -c "VACUUM (ANALYZE) kinds_map"
  meta orders "$(q -c 'SELECT count(*) FROM orders')"
  meta items "$(q -c "SELECT coalesce(sum(jsonb_array_length(items)), 0) FROM orders")"
  meta metrics "$(q -c 'SELECT count(*) FROM orders o, jsonb_object_keys(o.metrics)')"
  meta load_s "$(elapsed_s "$t0" "$(now_ms)")"
  meta stamp "$(stamp)"
  q -c CHECKPOINT
  log "loaded in $(meta_get load_s) s"
}

ensure_flat() {
  [[ -n $A_FLAT || -n $A_CH ]] || return 0
  [[ $(q -c "SELECT to_regclass('pg_items') IS NOT NULL") == t ]] || { log "flat tables"; q -f "$SQLD/flat.sql"; }
}

# the sift_col index of a table: the DDL is a parameter (A_COL_INDEX / --col-index, a statement or @FILE; the orders default is
# bench/sql/analytics/col_index.sql, hits has none). It is built again when the DDL changed (its hash is col_stamp of analytics_meta).
col_ddl() { # TABLE
  local d=$A_COL_INDEX
  [[ -n $d || $1 != orders ]] || d=@$SQLD/col_index.sql
  [[ $d != @* ]] || d=$(grep -v '^--' "${d#@}")
  echo "$d"
}
ensure_col() { # TABLE
  local table=$1 ddl t0 want
  ddl=$(col_ddl "$table")
  [[ -n $ddl ]] || { log "no sift_col DDL for $table: baseline only"; return 0; }
  q -c "SELECT count(*) FROM pg_available_extensions WHERE name = '$A_COL_EXT'" | grep -qx 1 || { log "extension $A_COL_EXT is not installed: baseline only"; return 0; }
  q -c "CREATE EXTENSION IF NOT EXISTS $A_COL_EXT"
  [[ $(q -c "SELECT count(*) FROM pg_am WHERE amname = '$A_COL_AM'") == 1 ]] || { log "this build of $A_COL_EXT has no $A_COL_AM access method: baseline only"; return 0; }
  want=$(echo "$ddl" | sha256sum | cut -c1-12)
  [[ $(meta_get col_stamp) != "$want" ]] || { log "$A_COL_AM index of $table is up to date"; return 0; }
  q -c "SELECT format('DROP INDEX %s;', i.indexrelid::regclass) FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid JOIN pg_am a ON a.oid = c.relam WHERE i.indrelid = '$table'::regclass AND a.amname = '$A_COL_AM'" | q -f -
  log "building the $A_COL_AM index of $table: $ddl"
  t0=$(now_ms)
  q -c "$ddl" -c "VACUUM (ANALYZE) $table"
  meta col_build_s "$(elapsed_s "$t0" "$(now_ms)")"
  meta col_stamp "$want"
}

# --- ClickHouse: a throwaway server of its own, loaded from the flat tables so that both engines hold the same rows
ch_up() { ch_client -q 'SELECT 1' >/dev/null 2>&1; }
ch_start() {
  ch_up && return
  [[ -x $CH_BIN ]] || { echo "analytics: $CH_BIN not found (CH_BIN)" >&2; return 1; }
  mkdir -p "$CH_DIR"/{data,tmp,user_files,logs}
  cat >"$CH_DIR/config.xml" <<EOF
<clickhouse>
  <logger><level>warning</level><log>$CH_DIR/logs/ch.log</log><errorlog>$CH_DIR/logs/err.log</errorlog><console>0</console></logger>
  <http_port>$CH_HTTP</http_port>
  <tcp_port>$CH_TCP</tcp_port>
  <listen_host>127.0.0.1</listen_host>
  <path>$CH_DIR/data/</path>
  <tmp_path>$CH_DIR/tmp/</tmp_path>
  <user_files_path>$CH_DIR/user_files/</user_files_path>
  <mark_cache_size>268435456</mark_cache_size>
  <max_server_memory_usage>8589934592</max_server_memory_usage>
  <query_log><database>system</database><table>query_log</table><flush_interval_milliseconds>1000</flush_interval_milliseconds></query_log>
  <timezone>UTC</timezone>
  <profiles><default/></profiles>
  <users><default><password></password><networks><ip>127.0.0.1</ip></networks><profile>default</profile><quota>default</quota></default></users>
  <quotas><default/></quotas>
</clickhouse>
EOF
  CLICKHOUSE_WATCHDOG_ENABLE=0 nice "$CH_BIN" server --config-file="$CH_DIR/config.xml" >"$CH_DIR/logs/stdout" 2>&1 8>&- &
  echo $! >"$CH_DIR/pid"
  for _ in $(seq 100); do ch_up && { log "clickhouse up on port $CH_TCP (pid $(<"$CH_DIR/pid"))"; return; }; sleep 0.3; done
  echo "analytics: clickhouse did not start, see $CH_DIR/logs" >&2; return 1
}
ch_stop() {
  [[ -f $CH_DIR/pid ]] || return 0
  kill "$(<"$CH_DIR/pid")" 2>/dev/null && while kill -0 "$(<"$CH_DIR/pid")" 2>/dev/null; do sleep 0.2; done
  rm -f "$CH_DIR/pid"
}
ch_copy() { q -c "COPY ($2) TO STDOUT" | ch_client -q "INSERT INTO $1 FORMAT TSV"; }
ensure_ch() {
  [[ -n $A_CH ]] || return 0
  ch_start
  [[ $(cat "$CH_DIR/stamp" 2>/dev/null) != "$(stamp)" ]] || { log "clickhouse tables are up to date"; return 0; }
  log "loading ClickHouse"
  ch_client --multiquery <"$SQLD/ch.sql"
  ch_copy items_tuned "SELECT tenant_id, customer_id, source, placed_at, order_id, idx, country_code, lang, channel, normalized::int, category, sku, kind, qty, price, discount, mhead, (deleted_at IS NOT NULL)::int FROM pg_items ORDER BY tenant_id, placed_at, order_id, idx"
  ch_copy metrics_tuned "SELECT tenant_id, customer_id, source, placed_at, order_id, country_code, lang, metric_key, value, (deleted_at IS NOT NULL)::int FROM pg_metrics ORDER BY tenant_id, placed_at, order_id"
  ch_copy metrics_map "SELECT source, metric_key, coalesce(derived_metric, ''), coalesce(mapped_aspect, '') FROM metrics_map"
  ch_copy categories_map "SELECT category, derived_category FROM categories_map"
  ch_copy kinds_map "SELECT kind, derived_kind FROM kinds_map"
  for t in items_tuned metrics_tuned; do ch_client -q "OPTIMIZE TABLE $t FINAL"; done
  stamp >"$CH_DIR/stamp"
}

setup() {
  build_sift
  cluster_start
  load
  ensure_flat
  ensure_col orders
  ensure_ch
}

# --- runs
py() { python3 -I "$STEPS/analytics.py" "$@"; }
common_args() { # the arguments of analytics.py that every corpus takes
  local a=(--out "$OUT" --runs "$A_RUNS" --col-am "$A_COL_AM" ${A_TIMEOUT:+--timeout "$A_TIMEOUT"} ${A_ENGINES:+--engines "$A_ENGINES"} ${A_QUERIES:+--queries "$A_QUERIES"})
  [[ -z $A_COL_PLAN ]] || a+=(--col-plan "$A_COL_PLAN")
  [[ -z $A_BASE_GUCS ]] || a+=(--base-gucs "$A_BASE_GUCS")
  [[ -z $A_COL_GUCS ]] || a+=(--col-gucs "$A_COL_GUCS")
  printf '%s\0' "${a[@]}"
}
A_COL_PLAN=${A_COL_PLAN:-} A_BASE_GUCS=${A_BASE_GUCS:-} A_COL_GUCS=${A_COL_GUCS:-}

describe() { # machine.txt of the result directory, with what the corpus is
  "$STEPS/machine.sh" >"$OUT/machine.txt"
  { echo "analytics corpus: $1"
    echo "clickhouse: $(if [[ -x $CH_BIN ]]; then "$CH_BIN" local --version 2>/dev/null | head -1; else echo not installed; fi)"
    echo "data directory: $(stat -f -c %T "$PGDATA") $(du -sh "$PGDATA" | cut -f1)"; } >>"$OUT/machine.txt"
}

cmd_run() {
  local args=()
  setup
  OUT=${OUT:?the result directory (bench/bench analytics run sets it)}
  mkdir -p "$OUT"
  describe "$(meta_get orders) orders, $(meta_get items) items, $(meta_get metrics) metrics, $A_STORES tenants${A_CLUSTER:+, clustered by $A_CLUSTER}"
  mapfile -d '' args < <(common_args)
  local ch=()
  [[ -z $A_CH ]] || ch=(--ch-bin "$CH_BIN" --ch-port "$CH_TCP" --ch-pid "$(<"$CH_DIR/pid")")
  [[ -z $A_SETS ]] || args+=(--sets "$A_SETS")
  py run --corpus orders --tier "$(awk -v n="$(meta_get orders)" 'BEGIN {printf (n >= 1e6 ? "%.1fM" : "%dk"), (n >= 1e6 ? n / 1e6 : n / 1e3)}')" "${args[@]}" "${ch[@]}"
}

# --- ClickBench: the 43 queries of ClickHouse/ClickBench (postgresql/queries.sql) on the first A_ROWS rows of hits (1M rows per file)
cb_fetch() { # name: a pinned file of the benchmark repository
  local f=$CB_DIR/$1.sql sum=CB_SHA256_$1
  mkdir -p "$CB_DIR"
  [[ -s $f ]] || curl -fsSL -m 120 -o "$f" "https://raw.githubusercontent.com/ClickHouse/ClickBench/$CB_COMMIT/postgresql/$1.sql"
  echo "${!sum}  $f" | sha256sum -c --quiet -
}
cb_load() {
  local files=$(( (A_ROWS + 999999) / 1000000 )) i left=$A_ROWS dir n
  if db_exists "$CB_DB" && [[ $(PGDATABASE=$CB_DB meta_get rows) == "$A_ROWS" ]]; then log "hits: $A_ROWS rows already loaded"; return; fi
  cb_fetch queries; cb_fetch create
  drop_db "$CB_DB"; create_db "$CB_DB"
  export PGDATABASE=$CB_DB
  q -f "$CB_DIR/create.sql" -c "CREATE TABLE analytics_meta (key text PRIMARY KEY, value text)"
  for (( i = 0; i < files; i++ )); do
    dir=$CB_DIR/hits_$i   # one new directory per download, only read by clickhouse local
    mkdir -p "$dir"
    [[ -s $dir/hits_$i.parquet ]] || curl -fsS -m 3600 -o "$dir/hits_$i.parquet" "$CB_BASE/hits_$i.parquet"
    n=$(( left < 1000000 ? left : 1000000 )); left=$(( left - n ))
    log "loading hits_$i.parquet ($n rows)"
    # the athena files hold dates as days and times as seconds since the epoch (Moscow time, the one that EventDate is in)
    "$CH_BIN" local -q "SELECT * REPLACE (toDate(EventDate) AS EventDate, toDateTime(EventTime, 'Europe/Moscow') AS EventTime, toDateTime(ClientEventTime, 'Europe/Moscow') AS ClientEventTime, toDateTime(LocalEventTime, 'Europe/Moscow') AS LocalEventTime) FROM file('$dir/hits_$i.parquet', Parquet) LIMIT $n FORMAT TSV" |
      q -c "COPY hits FROM STDIN"
    [[ -n ${KEEP_DOWNLOADS:-} ]] || rm -rf "$dir"
  done
  q -c "VACUUM (ANALYZE) hits" -c CHECKPOINT
  meta rows "$A_ROWS"
}
cb_setup() {
  [[ $A_BUILD == 0 || -z $A_COL_INDEX ]] || "$STEPS/fetch.sh" sift
  cluster_start
  cb_load
  ensure_col hits
}
cmd_clickbench() {
  local args=()
  PGDATABASE=$CB_DB cb_setup
  export PGDATABASE=$CB_DB
  OUT=${OUT:?the result directory (bench/bench analytics clickbench sets it)}
  mkdir -p "$OUT"
  describe "ClickBench hits, $(q -c 'SELECT count(*) FROM hits') rows (the first $(( (A_ROWS + 999999) / 1000000 )) partitions of hits_compatible), queries of ClickHouse/ClickBench@${CB_COMMIT:0:12}"
  mapfile -d '' args < <(common_args)
  py run --corpus clickbench --queries-file "$CB_DIR/queries.sql" --table hits --engines "${A_ENGINES:-heap,col}" --tier "$(( A_ROWS / 1000000 ))M" "${args[@]}"
}

cmd_clean() {
  ch_stop
  TIER=Q "$STEPS/cluster.sh" destroy
  rm -rf "$CH_DIR" "$ROOT/run" "$CB_DIR/hits_"*
  [[ -z ${A_ALL:-} ]] || rm -rf "${ROOT:?}" "$CB_DIR"
  log "removed the analytics cluster and ClickHouse$([[ -z ${A_ALL:-} ]] || echo ", the builds and the downloads")"
}

case ${1:-} in
  setup) A_FLAT=${A_FLAT:-}; setup ;;
  run) cmd_run ;;
  clickbench) cmd_clickbench ;;
  clickbench-setup) A_COL_INDEX=${A_COL_INDEX:-}; PGDATABASE=$CB_DB cb_setup ;;
  report) shift; py report "${1:-$BENCH/out/latest}" ;;
  clean) cmd_clean ;;
  *) echo "usage: $0 setup|run|clickbench|clickbench-setup|report|clean" >&2; exit 1 ;;
esac
