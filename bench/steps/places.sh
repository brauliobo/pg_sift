#!/usr/bin/env bash
# Multi-field autocomplete benchmark, every engine of bench/engines.tsv with a places adapter (PLACES_* and P_* of
# bench/engines/<engine>.sh) on GeoNames places (name, alternate names, region, country, feature): 1-4 letter prefixes, whole
# words, 2-3 word ANDs, name matches boosted x3, top-k by score, filtered and unordered scans, count(*).
# An engine without a native query for a workload (rum has no field boost, pg_textsearch no prefix scoring and no field boost,
# pgx-bm25 no wildcard shorter than 3 letters) builds an emulation next to its native index, in the same database, reported as
# the `emulation` build of the engine where it is separate; the adapter documents it. Every engine has a
# cluster of its own (a lane, steps/lanes.sh), with the places loaded and its index built one engine after the other (or taken from
# the cache of built lanes), so nothing of another engine's columns, hooks or
# access method (two extensions are called `bm25`) is in its database. Engines whose extension is missing are skipped.
#
#   bench/steps/places.sh quick|full [run|cold]    cities15000 (34k places) or cities500 (236k places)
#   bench/steps/places.sh quick|full check [oracle|rank] [N]   correctness against the SQL oracle of sql/places_oracle.sql
#
# Needs bench/bench setup (the engines, the places dataset). Every round times every cell (a workload on a query class) on every
# engine, one engine at a time with every CPU of the host to itself, the order of the engines rotating every round: a single client, p50 and p95 from the transaction latencies, pgbench seeded alike, measured by steps/cell.py until the
# relative standard error of the p50 is at most RSE (default 5%, order statistics) with at least MIN_TIME seconds and MIN_N
# transactions, and at most SECS seconds. After MINR rounds only the cells whose p50 has a relative standard error above RSE are
# measured again, up to MAXR rounds, unless the error of one round alone is so large that more rounds cannot reach RSE (steps/lib.sh).
# Compact tables only: $DEST (default $RESULTS/places-<mode>-<time>)/{cells,builds}.tsv (cold.tsv for cold), machine.txt and phases.txt.
# KEEP=1 keeps the lanes.
# Env: ENGINES CLASSES WLS SECS (the longest a cell is measured) MIN_TIME MIN_N MINR MAXR RSE NQ (cold queries per class; check: queries
# per class) TIMEOUT (statement_timeout of a cell, default 120s: slower cells are reported as error) NO_CACHE RESULTS DEST.
set -euo pipefail
mode=${1:-quick}; what=${2:-run}; check=${3:-all}
case $mode in
  quick) export TIER=Q; SECS=${SECS:-1} MINR=${MINR:-2} MAXR=${MAXR:-3}; CLASSES=${CLASSES:-p1 p3 w1 a2 a3 x2}; WLS=${WLS:-top boost filter count} ;;
  full)  export TIER=F; SECS=${SECS:-1} MINR=${MINR:-2} MAXR=${MAXR:-3}; CLASSES=${CLASSES:-p1 p2 p3 p4 w1 a2 a2s a3 x2}; WLS=${WLS:-top boost filter count scan} ;;
  *) sed -n '2,5p' "$0"; exit 1 ;;
esac
[[ $what != check ]] || export NO_CACHE=1   # the oracle tables live in the `places` database of every lane
. "$(dirname "$0")/lib.sh"
LANE_PORT_BASE=$(( ${TIER_PORT[$TIER]} + 50 ))
. "$STEPS/lanes.sh"
. "$STEPS/xlib.sh"
NQ=${NQ:-12} RSE=${RSE:-0.05} MIN_TIME=${MIN_TIME:-0.25} MIN_N=${MIN_N:-3}
[[ $what != check ]] || NQ=${4:-30}
DATADIR=$DATA/places-$([[ $TIER == Q ]] && echo quick || echo full)
RESULTS=${RESULTS:-$BENCH/out/ab}
DEST=${DEST:-$RESULTS/places-$mode-$(date +%m%d-%H%M)}; mkdir -p "$DEST" "$ROOT/places"
export LOG_TAG=places
[[ -f $DATADIR/places.tsv ]] || { echo "no $DATADIR: bench/datasets/fetch.sh places" >&2; exit 1; }

ENGINES=${ENGINES:-$DEFAULT_ENGINES}
edb() { echo "places_$1"; }
declare -A EXT   # the engines outside Postgres (steps/xlib.sh)
# shellcheck disable=SC2086  # one argument per engine
cleanup() {
  local e
  for e in "${!EXT[@]}"; do lane_run "$e" xe "$e" x_stop 2>/dev/null || true; done
  # shellcheck disable=SC2086  # one argument per engine
  [[ -n ${KEEP:-} ]] || { [[ -z ${LANE_PORT[*]+x} ]] || for e in $ENGINES; do lane_destroy "$e"; done; rm -rf "$ROOT/places"; }
}
trap cleanup EXIT

# the query forms (columns q sq sqb tq ng) of the pq rows, see engines/_places.sh; add a column here when an engine needs another
# rendering of the typed words
FORMS="SELECT q, sq, format('(%s) AND (name:{%s}^3 OR (%s))', sq, sq, sq) AS sqb, tq, place_ngram_terms(q) AS ng
       FROM (SELECT q, regexp_replace(q, '(\S+)', '\1*', 'g') AS sq,
                    (SELECT string_agg(quote_literal(lexeme) || ':*', ' & ') FROM unnest(to_tsvector('simple', q))) AS tq FROM pq"
base() { awk -F'\t' -v c="$1" '$2 == c && (m == "" || $1 < m) {m = $1} END {print m - 1}' "$DATADIR/queries.tsv"; }   # the id before the first query of a class

phase() { printf '%s\t%s\n' "$1" "$(( SECONDS - T0 ))" >>"$DEST/phases.txt"; T0=$SECONDS; }
T0=$SECONDS; : >"$DEST/phases.txt"
ENGINES=$(lanes_available places "$ENGINES" | tr '\n' ' ')
[[ -n $ENGINES ]] || { echo "no engine to run: bench/bench setup" >&2; exit 1; }
# the adapters of the engines, by engine (an adapter defines the same variable names)
declare -A STMT PSETUP PINDEX PINDEXES PSETUP2 PINDEX2 PINDEXES2 FLAVOR FLAVOR_MATCH
for e in $ENGINES; do
  load_engine "$e"
  [[ -z $EXTERNAL ]] || EXT[$e]=1
  PSETUP[$e]=$PLACES_SETUP PINDEX[$e]=$PLACES_INDEX PINDEXES[$e]=$PLACES_INDEXES
  PSETUP2[$e]=$PLACES_SETUP2 PINDEX2[$e]=$PLACES_INDEX2 PINDEXES2[$e]=$PLACES_INDEXES2
  FLAVOR[$e]=$PLACES_FLAVOR FLAVOR_MATCH[$e]=${PLACES_FLAVOR_MATCH:-$PLACES_FLAVOR}
  for k in match top boost filter count scan; do v=P_$k; STMT[$k/$e]=${!v}; done
done
stmt() { echo "${STMT[$1/$2]}"; }   # workload engine -> the statement (empty: the engine has no such query)
# shellcheck disable=SC2086  # one argument per engine
lanes_init places $ENGINES
"$STEPS/machine.sh" >"$DEST/machine.txt"
wc -l <"$DATADIR/places.tsv" >"$DEST/corpus.txt"   # the places of the tier, for the report
phase 'setup'

# --- the places and one engine's index, timed
timed_build() { # ENGINE VARIANT SETUP INDEX INDEXES: build row: engine, index build s, index bytes, table growth bytes (stored columns), setup s, variant
  local e=$1 db size=0 i heap0 t0 t1 t2
  db=$(edb "$e")
  heap0=$(PGDATABASE=$db q -c "SELECT pg_table_size('places')")
  t0=$(now_ms); PGDATABASE=$db q -c "$3" >/dev/null; t1=$(now_ms); PGDATABASE=$db q -c "$4" >/dev/null; t2=$(now_ms)
  PGDATABASE=$db q -c "VACUUM (ANALYZE) places" -c CHECKPOINT
  # shellcheck disable=SC2086  # one index per word
  for i in $5; do size=$(( size + $(PGDATABASE=$db q -c "SELECT pg_relation_size('$i'::regclass)") )); done
  printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$e" "$(elapsed_s "$t1" "$t2")" "$size" "$(( $(PGDATABASE=$db q -c "SELECT pg_table_size('places')") - heap0 ))" \
    "$(elapsed_s "$t0" "$t1")" "$2" >>"$RUN/builds.row"
}
ensure_places() { # once per cluster: the places table (a vacuumed template of every engine's database), the oracle tables for a check
  db_exists places && return 0
  PGDATABASE=postgres q -c "CREATE DATABASE places"
  PGDATABASE=places q -v dir="$DATADIR" -f "$BENCH/sql/places_schema.sql"
  PGDATABASE=places q -c "VACUUM (FREEZE, ANALYZE) places"   # every copy starts with a complete visibility map: count(*) of sift reads the heap of the pages that are not all-visible
  if [[ $what == check ]]; then PGDATABASE=places q -f "$BENCH/sql/places_oracle.sql" >/dev/null; PGDATABASE=places q -c "SELECT place_oracle_fill($NQ)" >/dev/null; fi
}
# what an engine outside Postgres needs rendered: the workloads and classes of the cells, those of the cold-cache cells
x_requests() { if [[ $what == cold ]]; then echo "top boost filter count|p1 p2 a2 a3"; else echo "$WLS|$CLASSES"; fi; }
prepare() { # ENGINE, in its lane: the places loaded, the engine's copy built (or the lane taken from the cache)
  local e=$1 key req
  export RUN=$ROOT/places/$e
  mkdir -p "$RUN"; : >"$RUN/builds.row"
  if [[ -n ${EXT[$e]:-} ]]; then   # a server of its own, built every time
    lane_start "$e"; ensure_places
    req=$(x_requests)
    xe "$e" xp_prepare "${req%|*}" "${req#*|}"
    xe "$e" x_warm "${XWARM:-1}" 1 "$RUN"/*.q   # the JVM and the caches warm: what the first execution of a statement is for Postgres
    return
  fi
  key=$(lane_key "$e" "$DATADIR/places.tsv" "$DATADIR/queries.tsv")
  if lane_cached "$e" "$key"; then
    lane_restore "$e" "$key"; lane_start "$e"; cp "$LANE_CACHE/$key/builds.row" "$RUN/builds.row"
    log "$e: built database from the cache ($key)"; return
  fi
  lane_start "$e"; ensure_places
  PGDATABASE=postgres q -c "DROP DATABASE IF EXISTS $(edb "$e") WITH (FORCE)" -c "CREATE DATABASE $(edb "$e") TEMPLATE places"
  timed_build "$e" - "${PSETUP[$e]}" "${PINDEX[$e]}" "${PINDEXES[$e]}"
  [[ -z ${PINDEX2[$e]} ]] || timed_build "$e" emulation "${PSETUP2[$e]}" "${PINDEX2[$e]}" "${PINDEXES2[$e]}"
  PGDATABASE=$(edb "$e") q -c "CREATE EXTENSION IF NOT EXISTS pg_buffercache" >/dev/null
  lane_save "$e" "$key" "$RUN/builds.row"
}
build_failed() { echo "build of $1 failed: $(tail -3 "$ROOT/places/$1.prepare.log")" >&2; exit 1; }
for e in $ENGINES; do lane_run "$e" prepare "$e" >"$ROOT/places/$e.prepare.log" 2>&1 || build_failed "$e"; done   # one after the other: a build is timed alone
: >"$DEST/builds.tsv"
for e in $ENGINES; do cat "$ROOT/places/$e/builds.row" >>"$DEST/builds.tsv"; done
log "built: $(awk '{printf "%s%s %.1fs %.0fMB; ", $1, $6 == "-" ? "" : "+" $6, $2, $3 / 1048576}' "$DEST/builds.tsv")"
phase 'restore and build'

if [[ $what == cold ]]; then
  # shellcheck disable=SC2329  # run by lane_run
  cold_lane() { # ENGINE, in its lane: the cold-cache times of every workload and class
    local e=$1 wl c b s t tq tsq tsqb ttq tng
    # shellcheck disable=SC2317  # run by cold_cell
    cold_query() { # i: the i-th query of the class
      IFS=$'\t' read -r tq tsq tsqb ttq tng < <(PGDATABASE=places q -F $'\t' -c "$FORMS WHERE id = $b + $1) x")
      PGDATABASE=$(edb "$e") cold_ms "$s" -v q="$tq" -v sq="$tsq" -v sqb="$tsqb" -v tq="$ttq" -v ng="$tng"
    }
    PGDATABASE=$(edb "$e") cold_table places
    for wl in top boost filter count; do for c in p1 p2 a2 a3; do
      b=$(base "$c")
      if [[ -n $(stmt "$wl" "$e") ]]; then
        s=$(stmt "$wl" "$e" | sed "s/:\(sqb\|sq\|tq\|ng\|q\)\b/:'\1'/g")
        t=$(cold_cell cold_query)
      else t=$'NA\tNA\t0'; fi
      printf '%s\t%s\t%s\t%s\n' "$wl" "$c" "$e" "$t"
    done; done >"$ROOT/places/$e/cold.part"
  }
  # shellcheck disable=SC2329  # run by lane_run
  cold_http() { # ENGINE, in its lane: an engine outside Postgres closes and opens its index before every request (steps/xlib.sh)
    local e=$1 wl c
    for wl in top boost filter count; do for c in p1 p2 a2 a3; do
      printf '%s\t%s\t%s\t%s\n' "$wl" "$c" "$e" "$(xe "$e" x_cold_class "$ROOT/places/$e/$wl.$c.q" places)"
    done; done >"$ROOT/places/$e/cold.part"
  }
  for e in $ENGINES; do
    fn=cold_lane; [[ -z ${EXT[$e]:-} ]] || fn=cold_http
    lane_run "$e" $fn "$e"
  done
  printf 'workload\tclass\tengine\tp50_ms\tmax_ms\tn\n' >"$DEST/cold.tsv"
  for wl in top boost filter count; do for c in p1 p2 a2 a3; do for e in $ENGINES; do grep -P "^$wl\t$c\t$e\t" "$ROOT/places/$e/cold.part" || true; done; done; done | tee -a "$DEST/cold.tsv"
  phase 'cold'
  exit 0
fi

if [[ $what == check ]]; then   # per engine and workload: the oracle's agreement per class (sql/places_oracle.sql), then a total
  case $check in oracle) wls=match ;; rank) wls="top boost filter" ;; *) wls="match top boost filter" ;; esac
  # what a word is for the engine's index (sql/places_oracle.sql): fold (accents and case), split (alphanumeric runs) or pg (the PG parser)
  flavor() { [[ $1 == match ]] && echo "${FLAVOR_MATCH[$2]}" || echo "${FLAVOR[$2]}"; }
  printf 'engine\tworkload\tqueries\texact\tagree\tworst\twrong\tshort\tagree per class\n' | tee "$DEST/check.txt"
  check_sql() { # ENGINE WORKLOAD FROM-ITEM: the row of the summary
    echo "SELECT '$1', '$2', sum(queries), sum(exact), round(sum(agree * queries) / sum(queries), 3), min(worst), sum(wrong), sum(short), string_agg(class || '=' || agree, ' ' ORDER BY class) FROM $3"
  }
  for e in $ENGINES; do for wl in $wls; do
    if [[ -n ${EXT[$e]:-} ]]; then   # the answers of the server, compared in the places database that holds the oracle
      ids=$(RUN=$ROOT/places/$e lane_run "$e" xe "$e" xp_ids "$wl" "$NQ")
      PGDATABASE=places lane_run "$e" q -F $'\t' -c "CREATE TEMP TABLE place_got (qid int, ids int[])" -c "\\copy place_got FROM '$ids'" \
        -c "$(check_sql "$e" "$wl" "place_check_ids('$wl', '$(flavor "$wl" "$e")')")" | tail -1 | tee -a "$DEST/check.txt"
      continue
    fi
    [[ -n $(stmt "$wl" "$e") ]] || continue
    PGOPTIONS="-c paradedb.planner_warnings=off" PGDATABASE=$(edb "$e") lane_run "$e" q -F $'\t' \
      -c "$(check_sql "$e" "$wl" "place_check('$wl', \$chk\$$(stmt "$wl" "$e")\$chk\$, '$(flavor "$wl" "$e")')")" | tee -a "$DEST/check.txt"
  done; done
  exit 0
fi

cell() { # WORKLOAD CLASS ENGINE -> "p50 p95 stderr-of-p50 transactions seconds" (ms, tab separated) of single-client transactions: one primary-key lookup of the query text plus the statement
  local e=$3 f=$ROOT/places/$3.sql
  if [[ -n ${EXT[$e]:-} ]]; then   # an engine outside Postgres: the request file of the workload and class, one client over HTTP
    [[ -f $ROOT/places/$e/$1.$2.q ]] || { printf 'NA\tNA\tNA\tNA\tNA\n'; return; }
    RUN=$ROOT/places/$e lane_run "$e" xe "$e" x_cell_se latency "$ROOT/places/$e/$1.$2.q"
    return
  fi
  [[ -n $(stmt "$1" "$e") ]] || { printf 'NA\tNA\tNA\tNA\tNA\n'; return; }   # the engine has no such query
  { printf '\\set i random(1, 300)\nSELECT quote_literal(q) AS q, quote_literal(sq) AS sq, quote_literal(sqb) AS sqb, quote_literal(tq) AS tq, quote_literal(ng) AS ng FROM (%s WHERE id = %d + :i) x) y \\gset\n' "$FORMS" "$(base "$2")"; echo "$(stmt "$1" "$e");"; } >"$f"
  PGOPTIONS="-c statement_timeout=${TIMEOUT:-120s}" lane_run "$e" python3 -I "$STEPS/cell.py" latency --rse "$RSE" --min-time "$MIN_TIME" --max-time "$SECS" \
    --min-n "$MIN_N" --hint "$ROOT/places/$e.$1.$2.hint" --tmp "$ROOT/places" -- -n -f "$f" -c 1 "$(edb "$e")"
}
group() { # WORKLOAD CLASS ENGINES...: the cell of every engine, one after the other
  local wl=$1 c=$2 e; shift 2
  batch "places $wl $c"
  for e in "$@"; do cell "$wl" "$c" "$e" >"$ROOT/places/$e.val"; done
}
printf 'round\tload\tworkload\tclass\tengine\tp50\tp95\tse\tn\tsecs\n' >"$DEST/cells.tsv"
stable() { [[ -z $(rse_cells "$DEST/cells.tsv" 3,4,5 6 8) ]]; }   # no cell needs another round
cell_engines() { # ROUND WORKLOAD CLASS: the engines to measure: all up to MINR rounds, then those whose p50 is above the error target
  # shellcheck disable=SC2086  # one argument per engine
  if (( $1 <= MINR )); then rotated "$1" $ENGINES
  else rse_cells "$DEST/cells.tsv" 3,4,5 6 8 | awk -F'\t' -v w="$2" -v c="$3" '$1 == w && $2 == c {print $3}'; fi
}
round() {
  local wl c e engs
  for wl in $WLS; do for c in $CLASSES; do
    engs=$(cell_engines "$1" "$wl" "$c" | tr '\n' ' ')
    [[ -n $engs ]] || continue
    # shellcheck disable=SC2086  # one argument per engine
    group "$wl" "$c" $engs
    for e in $engs; do printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$1" "$(loadavg)" "$wl" "$c" "$e" "$(cat "$ROOT/places/$e.val")" >>"$DEST/cells.tsv"; done
  done; done
}
log "start: $(quiet_report 2>&1)"
rounds round
phase 'measure'
log "end: $(noisy_report)"
log "results in $DEST ($(tr '\n\t' ' ;' <"$DEST/phases.txt"))"
