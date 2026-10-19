#!/usr/bin/env bash
# The articles benchmark: every engine of bench/engines.tsv (Postgres extensions and external servers alike) on one corpus.
#
#   bench/steps/articles.sh quick          Q tier (Q_DOCS articles, default 150k), cells of 0.25-1 s, 2-3 rounds
#   bench/steps/articles.sh full           F tier (218k articles), cells of 0.25-2 s, 2-3 rounds
#   bench/steps/articles.sh cold [quick]   cold-cache execution times (index and heap evicted before every query)
#
# Every engine has a cluster of its own (a lane, steps/lanes.sh), restored from the corpus dump and indexed one engine after the
# other (or taken from the cache of built lanes); the lanes then idle. Each round runs every class on every engine, one engine at a
# time with every CPU of the host to itself, the order of the engines rotating every round. A round is one cell per class and engine: 8 clients on the trace of the class (SAMPLE=N: only its
# first N statements in a fixed hash order, which biases heavy-tailed classes), pgbench seeded alike for
# every engine, measured by steps/cell.py until the relative standard error of its queries per second is at most RSE (default 5%,
# batch means) with at least MIN_TIME seconds and MIN_N queries, and at most SECS seconds (the first query of every client of a
# chunk is dropped, there is no separate warm-up).
# After MINR rounds only the cells whose relative standard error is above RSE are measured again, up to MAXR rounds, unless the
# error of one round alone is so large that more rounds cannot reach RSE (rse_cells in steps/lib.sh); bench/steps/tables.py turns
# the raw rounds into medians and standard errors.
# Classes (CLASSES): and or phrase mixed count filter join (the sampled substrings of the trace, top-10 unless count / filter /
# join), term prefix fuzzy (36 derived queries, sql/harness.sql), update (writers at UPDATE_RATE ops/s: 70% update, 15% insert,
# 15% delete, while the `mixed` readers run; the cell is the readers' queries per second, the writers' rate and the WAL written
# are `write_ops` and `wal_mib` rows). A class an engine cannot render is NA. Elasticsearch is driven by bench/xbench (steps/xlib.sh)
# through the same cells as the Postgres engines (steps/cell.py --xbench), after a warm-up of every class (XWARM seconds, default 3);
# Tantivy and the update class run for a fixed SECS seconds. Cold cache: Elasticsearch closes and opens its index before every
# request (xlib.sh); Tantivy has no cold cell.
# Only compact tables are kept: $DEST (default $RESULTS/<mode>-<time>)/{cells,builds}.tsv (cold.tsv for cold), machine.txt and
# phases.txt (wall time of the phases); the lanes are removed on exit (KEEP=1 keeps them), the corpus dump stays in $CACHE.
# Env: ENGINES CLASSES COLD (cold classes, default all but update) CLIENTS SECS (the longest a cell is measured) MIN_TIME MIN_N RSE
# WARM (seconds of the warm-up of external engines) MINR MAXR SAMPLE (0: the whole trace)
# NQ (cold queries per class) UPDATE_RATE WRITERS TIMEOUT (statement_timeout of a cell) THREADS (pgbench -j, default 4) NO_CACHE RESULTS DEST.
# Needs bench/bench setup (the engines, the dataset).
set -euo pipefail
mode=${1:-quick}; cold=
[[ $mode == cold ]] && { cold=1; mode=${2:-full}; }
case $mode in
  quick) export TIER=Q; SECS=${SECS:-2} WARM=${WARM:-1} MINR=${MINR:-2} MAXR=${MAXR:-3} ;;
  full)  export TIER=F; SECS=${SECS:-2} WARM=${WARM:-1} MINR=${MINR:-2} MAXR=${MAXR:-3} ;;
  *) sed -n '2,5p' "$0"; exit 1 ;;
esac
. "$(dirname "$0")/xlib.sh"
. "$STEPS/lanes.sh"
ENGINES=${ENGINES:-$DEFAULT_ENGINES}
CLASSES=${CLASSES:-and or phrase mixed count filter join}
CLIENTS=${CLIENTS:-8} NQ=${NQ:-12} UPDATE_RATE=${UPDATE_RATE:-1000} WRITERS=${WRITERS:-4} SAMPLE=${SAMPLE:-0}
RSE=${RSE:-0.05} MIN_TIME=${MIN_TIME:-0.25} MIN_N=${MIN_N:-$CLIENTS}
FIXED=$(awk -v s="$SECS" 'BEGIN {print s == int(s) ? s : int(s) + 1}')   # pgbench -T and the external load generator take whole seconds
RESULTS=${RESULTS:-$BENCH/out/ab}
OUT=$ROOT/articles/$TIER; rm -rf "$OUT"; mkdir -p "$OUT" "$RESULTS"   # the scratch directory: pgbench scripts, request files, build rows
DEST=${DEST:-$RESULTS/${cold:+cold-}$mode-$(date +%m%d-%H%M)}; mkdir -p "$DEST"
export OUT LOG_TAG=articles

for c in $CLASSES; do case $c in and|or|phrase|mixed|count|filter|join|term|prefix|fuzzy|update) ;; *) echo "unknown class $c" >&2; exit 1 ;; esac; done
CLASSES="$(for c in $CLASSES; do [[ $c == update ]] || printf '%s ' "$c"; done)$([[ " $CLASSES " != *" update "* ]] || echo update)"   # update last: it changes the documents

EXTERNALS=''
cleanup() {
  local e
  for e in $EXTERNALS; do lane_run "$e" xe "$e" x_stop 2>/dev/null || true; done
  # shellcheck disable=SC2086  # one argument per engine
  [[ -n ${KEEP:-} ]] || { [[ -z ${LANE_PORT[*]+x} ]] || for e in $ENGINES; do lane_destroy "$e"; done; rm -rf "$ROOT/articles"; }
}
trap cleanup EXIT

# --- Postgres engines: the database with the index, timed; one pgbench script per class (the statements are rendered per engine)
build_timed() {
  local t0 t1
  t0=$(now_ms); q -c "$SETUP" >/dev/null
  t1=$(now_ms); q -c "$INDEX" >/dev/null 2>"$RUN/build.log"
  echo $(( t1 - t0 )) $(( $(now_ms) - t1 )) >"$RUN/build.ms"
}
writer_script() {
  local n; n=$(q -c "SELECT max(id) FROM docs")
  cat >"$RUN/writer.pgb" <<EOF
\\set r random(1, 100)
\\set src random(1, $n)
\\set dst random(1, $n)
\\if :r <= 70
UPDATE docs d SET body = s.body, a_count = d.a_count + 1 FROM docs s WHERE s.id = :src AND d.id = :dst;
\\elif :r <= 85
INSERT INTO docs (id, body, category_id, created, a_count, url)
  SELECT nextval('docs_id_seq'), body, category_id, current_date, 0, url FROM docs WHERE id = :src;
\\else
DELETE FROM docs WHERE id = :dst;
\\endif
EOF
}
class_scripts() { # in the engine's database: a class the engine cannot render gets no script (NA); update reads like `mixed`
  local c wl n
  for c in $CLASSES; do
    wl=$c; [[ $c == update ]] && wl=$(q -c "SELECT wl FROM stmts WHERE wl IN ('mixed', 'or') ORDER BY wl = 'mixed' DESC LIMIT 1")
    n=$(q -c "SELECT count(*) FROM stmts WHERE wl = '$wl'")
    (( n > 0 )) || continue
    (( SAMPLE == 0 || n < SAMPLE )) || n=$SAMPLE   # the first SAMPLE statements of the class, in the hash order of sql/harness.sql
    printf '\\set i random(1, %d)\nSELECT stmt FROM stmts WHERE wl = '"'"'%s'"'"' AND n = :i \\gset\n:stmt\n' "$n" "$wl" >"$RUN/$c.pgb"
    [[ $c != update ]] || writer_script
  done
}
prewarm() {
  q -c "SELECT sum(pg_prewarm(c.oid)) FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
        WHERE n.nspname IN ('public', 'pg_toast') AND c.relkind IN ('r', 'i', 't')" >/dev/null
}
build_pg() ( # ENGINE, in its lane
  load_engine "$1"
  local db heap0 t1 t2 size
  db=$(engine_db "$1")
  adopt_db "$db"   # a lane with one engine keeps no second copy of the corpus
  heap0=$(q -c "SELECT pg_table_size('docs')")
  locked build_timed
  read -r t1 t2 <"$RUN/build.ms"
  q -c "VACUUM (ANALYZE) docs" -c "$RENDER"
  size=$(q -c "SELECT pg_total_relation_size('docs_fts')")
  printf '%s\t%s\t%s\t%.1f\t%.1f\t%s\t%s\n' "$TIER" "$1" "$(q -c 'SELECT count(*) FROM docs')" "${t1}e-3" "${t2}e-3" "$size" \
    "$(( $(q -c "SELECT pg_table_size('docs')") - heap0 ))" >"$RUN/builds.row"
  log "$1 built: setup ${t1}ms index ${t2}ms size $size, classes $(q -c 'SELECT bench_build_stmts()' | tr '\n' ' ')"
)
prepare() { # ENGINE, in its lane: the database restored from the corpus and built, or taken from the cache
  local e=$1 key
  RESTORE_JOBS=$(nproc); export RESTORE_JOBS RUN=$OUT/$e
  mkdir -p "$RUN"
  if is_external "$e"; then
    lane_start "$e"; db_exists bench || "$STEPS/corpus.sh" restore >/dev/null
    xe "$e" x_prepare; return
  fi
  key=$(lane_key "$e" "$DUMP")
  if lane_cached "$e" "$key"; then
    lane_restore "$e" "$key"; lane_start "$e"; cp "$LANE_CACHE/$key/builds.row" "$RUN/builds.row"
    log "$e: built database from the cache ($key)"
  else
    lane_start "$e"; db_exists bench || "$STEPS/corpus.sh" restore >/dev/null
    PGDATABASE=bench build_pg "$e"
    lane_save "$e" "$key" "$RUN/builds.row"
  fi
}
warm_http() { # ENGINE, in its lane: every class for a few seconds, the JVM and the caches of the server warm, what prewarm is for the relations
  local c f=()
  for c in $CLASSES; do [[ $c == update || ! -f $OUT/$1/$c.q ]] || f+=("$OUT/$1/$c.q"); done
  RUN=$OUT/$1 xe "$1" x_warm "${XWARM:-3}" "$CLIENTS" "${f[@]}"
}
finish() { # ENGINE, in its lane, once its database is built: the statements of the classes, the relations in shared buffers
  if ! is_external "$1"; then RUN=$OUT/$1; PGDATABASE=$(engine_db "$1") class_scripts; PGDATABASE=$(engine_db "$1") prewarm
  elif is_http "$1"; then warm_http "$1"; fi
}
prepare_lane() { prepare "$1"; finish "$1"; }

# --- one cell: queries per second of the clients (NA: the engine cannot run the class; 0: a client failed, e.g. the statement timeout)
pg_update_cell() { # ENGINE SECONDS: readers and writers together; leaves $RUN/update.wps and update.wal
  local db lsn0 o
  db=$(engine_db "$1"); lsn0=$(PGDATABASE=$db lane_run "$1" q -c "SELECT pg_current_wal_lsn()")
  PGOPTIONS="-c statement_timeout=${TIMEOUT:-60s}" lane_run "$1" pgbench -n -c "$WRITERS" -j 1 -T "$2" -R "$UPDATE_RATE" --random-seed=7 -f "$RUN/writer.pgb" "$db" >"$RUN/writer.out" 2>&1 &
  o=$(PGOPTIONS="-c statement_timeout=${TIMEOUT:-60s}" lane_run "$1" pgbench -n -c "$CLIENTS" -j "${THREADS:-4}" -T "$2" --random-seed=7 -f "$RUN/update.pgb" "$db" 2>&1) || o=''
  wait || true
  grep -oP 'tps = \K[0-9.]+(?= \(without)' "$RUN/writer.out" >"$RUN/update.wps" || echo 0 >"$RUN/update.wps"
  PGDATABASE=$db lane_run "$1" q -c "SELECT pg_wal_lsn_diff(pg_current_wal_lsn(), '$lsn0')" >"$RUN/update.wal"
  grep -oP 'tps = \K[0-9.]+(?= \(without)' <<<"$o" || echo 0
}
pg_cell() { # ENGINE CLASS SECONDS: queries per second of the clients, with its standard error, the queries and the seconds (tab separated)
  local e=$1
  [[ $2 == update ]] && { pg_update_cell "$e" "$3"; return; }
  PGOPTIONS="-c statement_timeout=${TIMEOUT:-60s}" lane_run "$e" python3 -I "$STEPS/cell.py" qps --rse "$RSE" --min-time "$MIN_TIME" --max-time "$SECS" \
    --min-n "$MIN_N" --hint "$RUN/$2.hint" --tmp "$RUN" -- -n -c "$CLIENTS" -j "${THREADS:-4}" -f "$RUN/$2.pgb" "$(engine_db "$e")"
}
declare -A EXT   # the external engines
cell() { # ENGINE CLASS SECONDS [warm]: only the external engines are warmed up, a Postgres cell drops the first query of its clients
  local RUN=$OUT/$1
  [[ -f $RUN/$2.pgb || -f $RUN/$2.q ]] || { echo NA; return; }
  if [[ -n ${EXT[$1]:-} ]]; then lane_run "$1" xe "$1" x_cell "$2" "$3" "${4:-}"; elif [[ -z ${4:-} ]]; then pg_cell "$@"; fi
}
group() { # CLASS SECONDS WARM ENGINES...: the cell of every engine, one after the other
  local c=$1 secs=$2 warm=$3 e; shift 3
  rm -f "$OUT"/*/update.wps "$OUT"/*/update.wal
  batch "articles $c"
  for e in "$@"; do cell "$e" "$c" "$secs" "$warm" >"$OUT/$e/val"; done
}

# --- run
T0=$SECONDS; : >"$DEST/phases.txt"
ENGINES=$(lanes_available articles "$ENGINES" | tr '\n' ' ')
[[ -n $ENGINES ]] || { echo "no engine to run: bench/bench setup" >&2; exit 1; }
# shellcheck disable=SC2086  # one argument per engine
lanes_init articles $ENGINES
"$STEPS/machine.sh" >"$DEST/machine.txt"
[[ -f $DUMP ]] || { "$STEPS/corpus.sh" load >/dev/null; "$STEPS/cluster.sh" destroy >/dev/null; }   # the lanes restore from the dump
for e in $ENGINES; do mkdir -p "$OUT/$e"; is_external "$e" && { EXT[$e]=1; EXTERNALS+=" $e"; }; done
phase 'dump'
build_failed() { echo "build of $1 failed: $(tail -3 "$OUT/$1/prepare.log")" >&2; exit 1; }
for e in $ENGINES; do lane_run "$e" prepare_lane "$e" >"$OUT/$e/prepare.log" 2>&1 || build_failed "$e"; done   # one after the other: a build is timed alone
: >"$DEST/builds.tsv"
for e in $ENGINES; do cat "$OUT/$e/builds.row" >>"$DEST/builds.tsv"; done
log "built $ENGINES: $(awk '{printf "%s %.1fs %.0fMB; ", $2, $5, $6 / 1048576}' "$DEST/builds.tsv")"
awk -F'\t' 'NR == 1 {print $3}' "$DEST/builds.tsv" >"$DEST/corpus.txt"   # the articles of the tier, for the report
phase 'restore and build'

if [[ $cold ]]; then
  # shellcheck disable=SC2329  # run by lane_run
  cold_lane() { # ENGINE, in its lane: the cold-cache times of every class
    local e=$1 c t
    # shellcheck disable=SC2317  # run by cold_cell
    cold_stmt() { cold_ms "$(q -c "SELECT stmt FROM stmts WHERE wl = '$c' AND n = $1")"; }   # the first statements of the class, in the order of the trace
    export PGDATABASE; PGDATABASE=$(engine_db "$e"); RUN=$OUT/$e
    q -c "CREATE EXTENSION IF NOT EXISTS pg_buffercache" >/dev/null
    cold_table docs
    for c in ${COLD:-${CLASSES/update/}}; do
      if [[ -f $RUN/$c.pgb ]]; then t=$(cold_cell cold_stmt); else t=$'NA\tNA\t0'; fi
      printf -- '-\t%s\t%s\t%s\n' "$c" "$e" "$t"
    done >"$RUN/cold.part"
  }
  # shellcheck disable=SC2329  # run by lane_run
  cold_http() { # ENGINE, in its lane: the cold-cache times of an HTTP engine (the index closed and opened before every request, xlib.sh); the other external engines have no cold cell
    local e=$1 c t; RUN=$OUT/$e
    for c in ${COLD:-${CLASSES/update/}}; do
      if is_http "$e" && [[ -f $RUN/$c.q ]]; then t=$(xe "$e" x_cold_class "$RUN/$c.q" docs); else t=$'NA\tNA\t0'; fi
      printf -- '-\t%s\t%s\t%s\n' "$c" "$e" "$t"
    done >"$RUN/cold.part"
  }
  for e in $ENGINES; do
    fn=cold_lane; [[ -z ${EXT[$e]:-} ]] || fn=cold_http
    lane_run "$e" $fn "$e"
  done
  printf 'workload\tclass\tengine\tp50_ms\tmax_ms\tn\n' >"$DEST/cold.tsv"
  for c in ${COLD:-${CLASSES/update/}}; do for e in $ENGINES; do grep -P "^-\t$c\t$e\t" "$OUT/$e/cold.part" 2>/dev/null || true; done; done | tee -a "$DEST/cold.tsv"
  phase 'cold'
  exit 0
fi

printf 'round\tload\tclass\tengine\tmetric\tvalue\tse\tn\tsecs\n' >"$DEST/cells.tsv"
row() { printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$1" "$(loadavg)" "$2" "$3" "$4" "$5" "${6:-NA}" "${7:-NA}" "${8:-NA}" >>"$DEST/cells.tsv"; }   # round class engine metric value [stderr n seconds]
qps_cells() { awk -F'\t' 'NR == 1 || $5 == "qps"' "$DEST/cells.tsv"; }
stable() { [[ -z $(rse_cells <(qps_cells) 3,4 6 7) ]]; }   # no cell needs another round
cell_engines() { # ROUND CLASS: the engines to measure: all up to MINR rounds, then those whose cell is above the error target
  # shellcheck disable=SC2086  # one argument per engine
  if (( $1 <= MINR )); then rotated "$1" $ENGINES
  else rse_cells <(qps_cells) 3,4 6 7 | awk -F'\t' -v c="$2" '$1 == c {print $2}'; fi
}
round() {
  local c e engs v se n t
  for c in $CLASSES; do
    engs=$(cell_engines "$1" "$c" | tr '\n' ' ')
    [[ -n $engs ]] || continue
    # shellcheck disable=SC2086  # one argument per engine
    { [[ $1 != 1 || -z $EXTERNALS ]] || group "$c" "$WARM" warm $engs; group "$c" "$FIXED" '' $engs; }
    for e in $engs; do
      IFS=$'\t' read -r v se n t <"$OUT/$e/val" || true
      row "$1" "$c" "$e" qps "$v" "$se" "$n" "$t"
      [[ $c != update || $v == NA ]] || {
        row "$1" "$c" "$e" write_ops "$(cat "$OUT/$e/update.wps" 2>/dev/null || echo 0)"
        row "$1" "$c" "$e" wal_mib "$(awk -v b="$(cat "$OUT/$e/update.wal" 2>/dev/null || echo 0)" 'BEGIN {printf "%.1f", b / 1048576}')"
      }
    done
  done
}
log "start: $(quiet_report 2>&1)"
rounds round
phase 'measure'
log "end: $(noisy_report)"
log "results in $DEST ($(tr '\n\t' ' ;' <"$DEST/phases.txt"))"
