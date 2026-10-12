#!/usr/bin/env bash
# Usage: TIER=S|M|L bench/run.sh ENGINE [WORKLOAD...]
#   ENGINE: gin rum pg_textsearch bm25 sift (bench/engines/*.sh)
#   WORKLOAD: and or phrase mixed count filter join update (default: all)
# Env: DURATION (s, default 30) CLIENTS (8) THREADS (4) WARMUP (s, 5) UPDATE_RATE (1000)
#      WRITERS (4) TIMEOUT (statement_timeout, 120s) KEEP=1 keeps the engine database.
. "$(dirname "$0")/lib.sh"

ENGINE=${1:?engine}; shift
WORKLOADS=${*:-and or phrase mixed count filter join update}
DURATION=${DURATION:-30} CLIENTS=${CLIENTS:-8} THREADS=${THREADS:-4} WARMUP=${WARMUP:-5}
UPDATE_RATE=${UPDATE_RATE:-1000} WRITERS=${WRITERS:-4}
export PGOPTIONS="-c statement_timeout=${TIMEOUT:-120s}"
load_engine "$ENGINE"
EDB=$(engine_db "$ENGINE")
RUN=$OUT/$ENGINE
mkdir -p "$RUN"

relsize() { q -c "SELECT pg_total_relation_size('docs_fts')"; }

build_timed() {
  local t0 t1
  t0=$(now_ms); q -c "$SETUP" >/dev/null
  t1=$(now_ms); q -c "$INDEX" >/dev/null 2>"$RUN/build.log"
  echo $(( t1 - t0 )) $(( $(now_ms) - t1 )) >"$RUN/build.ms"
}

build() {
  clone_db "$EDB"
  local heap0 t1 t2
  heap0=$(q -c "SELECT pg_table_size('docs')")
  locked build_timed
  read -r t1 t2 <"$RUN/build.ms"
  q -c "VACUUM (ANALYZE) docs" -c "$RENDER"
  printf '%s\t%s\t%s\t%.1f\t%.1f\t%s\t%s\n' "$TIER" "$ENGINE" "$(q -c 'SELECT count(*) FROM docs')" \
    "${t1}e-3" "${t2}e-3" "$(relsize)" "$(( $(q -c "SELECT pg_table_size('docs')") - heap0 ))" >>"$OUT/builds.tsv"
  log "$ENGINE built: setup ${t1}ms index ${t2}ms size $(relsize)"
}

prewarm() {
  q -c "SELECT sum(pg_prewarm(c.oid)) FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
        WHERE n.nspname IN ('public', 'pg_toast') AND c.relkind IN ('r', 'i', 't')" >/dev/null
}

reader_script() {
  local n; n=$(q -c "SELECT count(*) FROM stmts WHERE wl = '$1'")
  printf '\\set i random(1, %d)\nSELECT stmt FROM stmts WHERE wl = '\''%s'\'' AND n = :i \\gset\n:stmt\n' "$n" "$1" >"$RUN/$1.pgb"
}

writer_script() {
  cat >"$RUN/writer.pgb" <<EOF
\\set r random(1, 100)
\\set src random(1, $(q -c "SELECT max(id) FROM docs"))
\\set dst random(1, $(q -c "SELECT max(id) FROM docs"))
\\if :r <= 70
UPDATE docs d SET body = s.body, a_count = d.a_count + 1 FROM docs s WHERE s.id = :src AND d.id = :dst;
\\elif :r <= 85
INSERT INTO docs (id, body, prop_id, score, country, created, a_count, url)
  SELECT nextval('docs_id_seq'), body, prop_id, score, country, current_date, 0, url FROM docs WHERE id = :src;
\\else
DELETE FROM docs WHERE id = :dst;
\\endif
EOF
}

# Latency stats (ms) from pgbench per-transaction logs: avg p50 p95 p99 count.
latency() {
  cat "$@" 2>/dev/null | awk '{print $3}' | sort -n | awk '{a[NR] = $1; s += $1}
    function p(q) { k = int(NR * q + 0.999999); return a[k < 1 ? 1 : k] / 1000 }
    END { if (NR) printf "%.2f\t%.2f\t%.2f\t%.2f\t%d", s / NR / 1000, p(.5), p(.95), p(.99), NR; else printf "\t\t\t\t0" }'
}

mb_per_query() {
  q -c "SELECT round(sum(shared_blks_hit + shared_blks_read) * 8192 / 1e6 / nullif(sum(calls), 0), 2)
        FROM pg_stat_statements WHERE dbid = (SELECT oid FROM pg_database WHERE datname = current_database())
        AND query ~ '^SELECT' AND query ~ ' FROM docs' AND query !~ 'FROM stmts'"
}

pgb() { (cd "$RUN" && pgbench -n -c "$1" -j "$2" -T "$3" "${@:4}"); }

# Runs inside the perf lock: warm-up, then the timed reader run (and writers for `update`).
timed_run() {
  local wl=$1 rwl=$2
  rm -f "$RUN"/*.log.?*
  pgb "$CLIENTS" "$THREADS" "$WARMUP" -f "$RUN/$rwl.pgb" >/dev/null 2>&1 || true
  q -c "SELECT pg_stat_statements_reset()" >/dev/null
  local lsn0; lsn0=$(q -c "SELECT pg_current_wal_lsn()")
  if [[ $wl == update ]]; then
    pgb "$WRITERS" 2 "$DURATION" -R "$UPDATE_RATE" -l --log-prefix=writer.log -f "$RUN/writer.pgb" >"$RUN/writer.out" 2>&1 &
  fi
  pgb "$CLIENTS" "$THREADS" "$DURATION" -l --log-prefix="$wl.log" -f "$RUN/$rwl.pgb" >"$RUN/$wl.out" 2>&1 || true
  wait
  q -c "SELECT pg_wal_lsn_diff(pg_current_wal_lsn(), '$lsn0')" >"$RUN/$wl.wal"
  mb_per_query >"$RUN/$wl.mbq"
}

measure() {
  local wl=$1 rwl=$1
  if [[ $wl == update ]]; then
    rwl=$(q -c "SELECT wl FROM stmts WHERE wl IN ('mixed', 'or') ORDER BY wl = 'mixed' DESC LIMIT 1")
    writer_script
  fi
  if [[ -z $rwl || $(q -c "SELECT count(*) FROM stmts WHERE wl = '$rwl'") == 0 ]]; then
    printf '%s\t%s\t%s\t\t\t\t\t\t\t\t\t\t\t\tunsupported\n' "$TIER" "$ENGINE" "$wl" >>"$OUT/results.tsv"
    log "$ENGINE $wl: unsupported"; return
  fi
  reader_script "$rwl"
  locked timed_run "$wl" "$rwl"
  local qps aborted note
  qps=$(grep -oP 'tps = \K[0-9.]+(?= \(without)' "$RUN/$wl.out" || echo 0)
  aborted=$(grep -c 'aborted in command' "$RUN/$wl.out" || true)
  note=$([[ $aborted -gt 0 ]] && echo "$aborted/$CLIENTS clients aborted: $(grep -m1 -oP 'ERROR:\s+\K.*' "$RUN/$wl.out")" || true)
  [[ $wl == update ]] && note="reads=$rwl writes=$(latency "$RUN"/writer.log* | cut -f5) writer_tps=$(grep -oP 'tps = \K[0-9.]+(?= \(without)' "$RUN/writer.out") writer_p99=$(latency "$RUN"/writer.log* | cut -f4) $note"
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$TIER" "$ENGINE" "$wl" "$CLIENTS" "$DURATION" "$qps" \
    "$(latency "$RUN/$wl".log*)" "$(cat "$RUN/$wl.mbq")" "$(cat "$RUN/$wl.wal")" "$(cut -d' ' -f1 /proc/loadavg)" "$note" >>"$OUT/results.tsv"
  log "$ENGINE $wl: qps=$qps $note"
}

engine_available || { log "$ENGINE: extension $REQUIRES not installed, skipping"; exit 0; }
build
log "$ENGINE workloads: $(q -c 'SELECT bench_build_stmts()' | tr '\n' ' ')"
prewarm
for wl in $WORKLOADS; do measure "$wl"; done
[[ ${KEEP:-} ]] || PGDATABASE=postgres q -c "DROP DATABASE $EDB WITH (FORCE)"
