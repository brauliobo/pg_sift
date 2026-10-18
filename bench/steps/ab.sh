#!/usr/bin/env bash
# Interleaved A/B of sift builds on the slim corpus. One backend runs a class (or any pgbench script) while perf counts its user-space
# cycles and instructions per transaction: those hold up on a shared host where throughput does not. Builds are told apart by
# dynamic_library_path, one library per label; the on-disk format is the same, so one database serves them all.
#
#   bench/steps/ab.sh prepare                          cluster, slim corpus, index and the class scripts ($AB/pgb/CLASS.pgb)
#   bench/steps/ab.sh prepare_places                   the places database of the adapter with a btree on country_code (ABDB=places_sift)
#   bench/steps/ab.sh shapes                           the filter shapes of the plan-choice tests as scripts shape_NAME.pgb (ABDB=shapes)
#   bench/steps/ab.sh filters                          prepared ranked queries with a filter that keeps 0.1, 2.3, 10 and 50% of the rows
#   bench/steps/ab.sh filter_matrix REPS SECS|Nt LABEL...   run of those scripts, each label with the plan cache free, custom only, generic only
#   bench/steps/ab.sh build LABEL [REF]                release library of git REF (default HEAD; "work": the working tree) -> $AB/libs/LABEL
#   bench/steps/ab.sh run CLASS REPS SECS|Nt LABEL...  cycles and instructions per transaction, one backend, mean +- standard error (Nt: N transactions
#                                                      of the seeded sequence of statements, the same for every build)
#   bench/steps/ab.sh clients CLASS REPS SECS N LABEL...   transactions per second of N clients, same interleaving
#   LABEL=L bench/steps/ab.sh psql [ARGS...]           psql into the database, with the library of label L (default main)
#   bench/steps/ab.sh stop                             stop the cluster
#
# CLASS is a class of the articles trace (and or phrase mixed count filter join ...) or the path of a pgbench script. A LABEL may
# carry settings for the session: LABEL:guc=value,guc=value (e.g. base:sift.filter_pushdown=off). Environment: BENCH_WORK (default
# /tmp/sift-ab), BENCH_STORE (the data directory holds articles.tsv), PORT (default 56110, within 56100-56169), PGB (extra pgbench options).
export BENCH_WORK=${BENCH_WORK:-/tmp/sift-ab} PORT=${PORT:-56110} TIER=Q Q_DOCS=${Q_DOCS:-50000}
. "$(dirname "$0")/lib.sh"
AB=$ROOT/ab
ABDB=${ABDB:-ab}

prepare() {
  "$STEPS/fetch.sh" sift
  "$STEPS/corpus.sh" ensure
  load_engine sift
  clone_db "$ABDB"
  q -c "$SETUP" -c "$INDEX" -c "VACUUM (ANALYZE) docs" -c "$RENDER" -c "SELECT bench_build_stmts()" >/dev/null
  mkdir -p "$AB/pgb"
  q <<'SQL'
DROP TABLE IF EXISTS qtext;
CREATE TABLE qtext AS SELECT wl, n, (regexp_match(stmt, '@@> ''([^'']*)'''))[1] AS q FROM stmts WHERE stmt ~ '@@> ''';
ALTER TABLE qtext ADD PRIMARY KEY (wl, n);
ANALYZE qtext;
SQL
  local c n
  for c in $(q -c "SELECT DISTINCT wl FROM stmts"); do
    n=$(q -c "SELECT count(*) FROM stmts WHERE wl = '$c'")
    printf '\\set i random(1, %d)\nSELECT stmt FROM stmts WHERE wl = '"'"'%s'"'"' AND n = :i \\gset\n:stmt\n' "$n" "$c" >"$AB/pgb/$c.pgb"
    # the same statement with the query as a parameter (the _prep scripts run with -M prepared)
    printf '\\set i random(1, %d)\nSELECT q FROM qtext WHERE wl = '"'"'%s'"'"' AND n = :i \\gset\n%s;\n' "$n" "$c" \
      "$(q -c "SELECT regexp_replace(stmt, '@@> ''[^'']*''', '@@> :q') FROM stmts WHERE wl = '$c' AND n = 1")" >"$AB/pgb/${c}_prep.pgb"
  done
  q -c "SELECT sum(pg_prewarm(c.oid)) FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
        WHERE n.nspname IN ('public', 'pg_toast') AND c.relkind IN ('r', 'i', 't')" >/dev/null
  log "ab: $AB/pgb: $(ls "$AB/pgb" | tr '\n' ' ')"
}

# The places database of the sift adapter (cities15000) with a btree on country_code, which the filter workload of the adapter
# has no index for: a bitmap path exists, so the planner chooses between the pushdown and the post-filter scan.
# Scripts places_WORKLOAD_CLASS.pgb (run with ABDB=places_sift); PLACES_BTREE=0 leaves the btree out.
prepare_places() {
  "$STEPS/fetch.sh" sift
  "$STEPS/cluster.sh" start
  load_engine sift
  local db=places_sift wl c n
  PGDATABASE=postgres q -c "DROP DATABASE IF EXISTS $db WITH (FORCE)" -c "CREATE DATABASE $db"
  PGDATABASE=$db q -v dir="${PLACES_DIR:-$DATA/places-quick}" -f "$BENCH/sql/places_schema.sql"
  PGDATABASE=$db q -c "$PLACES_SETUP" -c "$PLACES_INDEX"
  [[ ${PLACES_BTREE:-1} == 0 ]] || PGDATABASE=$db q -c "CREATE INDEX places_country ON places (country_code)"
  PGDATABASE=$db q -c "VACUUM (FREEZE, ANALYZE) places" -c "CREATE TABLE pqs AS SELECT class, row_number() OVER (PARTITION BY class ORDER BY id) AS n,
    regexp_replace(q, '(\\S+)', '\\1*', 'g') AS sq FROM pq" -c "ALTER TABLE pqs ADD PRIMARY KEY (class, n)" -c "ANALYZE pqs"
  mkdir -p "$AB/pgb"
  for wl in top filter count; do
    for c in $(PGDATABASE=$db q -c "SELECT DISTINCT class FROM pqs"); do
      n=$(PGDATABASE=$db q -c "SELECT count(*) FROM pqs WHERE class = '$c'")
      v=P_$wl
      printf '\\set i random(1, %d)\nSELECT quote_literal(sq) AS sq FROM pqs WHERE class = '"'"'%s'"'"' AND n = :i \\gset\n%s;\n' "$n" "$c" "${!v}" >"$AB/pgb/places_${wl}_$c.pgb"
    done
  done
  log "ab: places in $db: $(ls "$AB/pgb" | grep -c '^places_') scripts"
}

# The filter shapes of the plan-choice tests (sql/ab_shapes.sql) in a database of their own (ABDB=shapes), one script shape_NAME.pgb each:
# time the planner's choice against the pushdown off and forced (main, main:sift.filter_pushdown=off, main:sift.filter_pushdown=force)
prepare_shapes() {
  "$STEPS/fetch.sh" sift
  "$STEPS/cluster.sh" start
  load_engine sift
  PGDATABASE=postgres q -c "DROP DATABASE IF EXISTS shapes WITH (FORCE)" -c "CREATE DATABASE shapes"
  PGDATABASE=shapes q -c "$SETUP" -f "$BENCH/sql/ab_shapes.sql"
  mkdir -p "$AB/pgb"
  PGDATABASE=shapes q -F $'\t' -c "SELECT name, stmt FROM shapes" | while IFS=$'\t' read -r name stmt; do echo "$stmt;" >"$AB/pgb/shape_$name.pgb"; done
  log "ab: shapes in database shapes: $(ls "$AB/pgb" | grep -c '^shape_') scripts"
}

# The ranked queries of the classes and, or and filter with a filter on `created` that keeps 0.1, 2.3, 10 and 50% of the rows, as prepared
# statements: scripts sel_CLASS_PCT_prep.pgb. Run them with the labels L, L:plan_cache_mode=force_custom_plan and
# L:plan_cache_mode=force_generic_plan to see the generic plan against the custom plans.
prepare_filters() {
  local wl pct day n
  for wl in and or filter; do
    n=$(PGDATABASE=$ABDB q -c "SELECT count(*) FROM qtext WHERE wl = '$wl'")
    for pct in 0.1 2.3 10 50; do
      day=$(PGDATABASE=$ABDB q -c "SELECT (percentile_disc(1 - $pct / 100.0) WITHIN GROUP (ORDER BY created))::date FROM docs")
      printf '\\set i random(1, %d)\nSELECT q FROM qtext WHERE wl = '"'"'%s'"'"' AND n = :i \\gset\nSELECT docs.id FROM docs WHERE docs.body @@> :q AND docs.created >= '"'"'%s'"'"' ORDER BY sift.score(docs.ctid) DESC LIMIT 10;\n' \
        "$n" "$wl" "$day" >"$AB/pgb/sel_${wl}_${pct}_prep.pgb"
    done
  done
  log "ab: $(ls "$AB/pgb" | grep -c '^sel_') filter scripts"
}

# every sel_* script with each LABEL as it is (the plan cache decides), with custom plans only and with the generic plan only
filter_matrix() { # REPS SECS|Nt LABEL...
  local reps=$1 secs=$2 f l labels
  shift 2
  for f in "$AB"/pgb/sel_*_prep.pgb; do
    labels=()
    for l in "$@"; do labels+=("$l" "$l:plan_cache_mode=force_custom_plan" "$l:plan_cache_mode=force_generic_plan"); done
    run "$f" "$reps" "$secs" "${labels[@]}"
  done
}

build() { # LABEL [REF]
  local label=$1 ref=${2:-HEAD} src=$AB/src/$1
  rm -rf "$src"; mkdir -p "$src" "$AB/libs/$label"
  if [[ $ref == work ]]; then git -C "$REPO" ls-files -z | tar -C "$REPO" --null -T - -cf - | tar -C "$src" -xf -
  else git -C "$REPO" archive "$ref" | tar -C "$src" -xf -; fi
  nice make -C "$src" -j4 USE_PGXS=1 SIFT_TESTING= >/dev/null
  cp "$src/pg_sift.so" "$AB/libs/$label/"
  rm -rf "$src"
  log "ab: $label = $ref ($(git -C "$REPO" rev-parse --short "$([[ $ref == work ]] && echo HEAD || echo "$ref")"))"
}

script() { [[ -f $1 ]] && echo "$1" || echo "$AB/pgb/$1.pgb"; }
# the _prep scripts bind the query as a parameter: prepared statements, whose generic plans the planner costs without the query
protocol() { [[ $1 == *_prep* ]] && echo "-M prepared"; return 0; }

# the PGOPTIONS of LABEL[:guc=value,...]
options() {
  local opts="-c dynamic_library_path=$AB/libs/${1%%:*}:\$libdir" g gs=()
  [[ $1 != *:* ]] || IFS=, read -ra gs <<<"${1#*:}"
  for g in "${gs[@]}"; do opts+=" -c $g"; done
  echo "$opts"
}

# the backend of the running pgbench $1 (its application_name), once it exists
backend() {
  local pid=''
  until [[ -n $pid ]]; do pid=$(PGAPPNAME=lookup psql -X -Atc "SELECT pid FROM pg_stat_activity WHERE application_name = '$1' LIMIT 1" "$ABDB"); done
  echo "$pid"
}

# mean +- standard error of the numbers in $1
stat() { echo "$1" | awk '{n = NF; s = 0; for (i = 1; i <= n; i++) s += $i; m = s / n; v = 0; for (i = 1; i <= n; i++) v += ($i - m) ^ 2; printf "%.0f +- %.0f", m, (n > 1) ? sqrt(v / (n - 1) / n) : 0}'; }

run() { # CLASS REPS SECS|Nt LABEL...
  local name=$1 c=$(script "$1") reps=$2 secs=$3 r l pid cyc ins tps out dur txns per
  shift 3
  # Nt: every run is N transactions of the same statements (the seeded sequence), so the builds meet the same queries;
  # SECS: a window of that many seconds of a run that goes on, whose statements differ with the speed
  if [[ $secs == *t ]]; then txns=${secs%t} dur="-t $txns"; else dur="-T $((secs + 3))"; fi
  declare -A cy in tp
  for r in $(seq "$reps"); do
    for l in "$@"; do
      out=$(mktemp)
      PGAPPNAME=ab PGOPTIONS=$(options "$l") pgbench -n -c 1 -j 1 $dur --random-seed=7 $(protocol "$c") ${PGB:-} -f "$c" "$ABDB" >"$out" 2>&1 &
      pid=$(backend ab)
      if [[ -n ${txns:-} ]]; then
        read -r cyc ins < <(perf stat -x, -e cycles:u,instructions:u -p "$pid" 2>&1 | awk -F, '/cycles/ {c = $1} /instructions/ {i = $1} END {print c, i}')
        per=$txns
      else
        read -r cyc ins < <(perf stat -x, -e cycles:u,instructions:u -p "$pid" -- sleep "$secs" 2>&1 | awk -F, '/cycles/ {c = $1} /instructions/ {i = $1} END {print c, i}')
        per=$(grep -oP 'tps = \K[0-9.]+(?= \(without)' "$out" | awk -v s="$secs" '{printf "%.6f", $1 * s}')
      fi
      wait
      tps=$(grep -oP 'tps = \K[0-9.]+(?= \(without)' "$out"); rm -f "$out"
      [[ -n ${txns:-} ]] || per=$(awk -v t="$tps" -v s="$secs" 'BEGIN {printf "%.6f", t * s}')
      cy[$l]+="$(awk -v c="$cyc" -v n="$per" 'BEGIN {printf "%.0f", c / n}') "
      in[$l]+="$(awk -v c="$ins" -v n="$per" 'BEGIN {printf "%.0f", c / n}') "
      tp[$l]+="$tps "
    done
  done
  echo "class=$name reps=$reps secs=$secs load=$(cut -d' ' -f1-3 /proc/loadavg)"
  for l in "$@"; do printf '  %-28s cycles/txn %-20s instr/txn %-20s tps %s\n' "$l" "$(stat "${cy[$l]}")" "$(stat "${in[$l]}")" "$(stat "${tp[$l]}")"; done
}

clients() { # CLASS REPS SECS N LABEL...
  local name=$1 c=$(script "$1") reps=$2 secs=$3 n=$4 r l out
  shift 4
  declare -A tp
  for r in $(seq "$reps"); do
    for l in "$@"; do
      out=$(PGOPTIONS=$(options "$l") pgbench -n -c "$n" -j 4 -T "$secs" --random-seed=7 $(protocol "$c") ${PGB:-} -f "$c" "$ABDB" 2>&1)
      tp[$l]+="$(grep -oP 'tps = \K[0-9.]+(?= \(without)' <<<"$out") "
    done
  done
  echo "class=$name reps=$reps secs=$secs clients=$n load=$(cut -d' ' -f1-3 /proc/loadavg)"
  for l in "$@"; do printf '  %-28s qps %s\n' "$l" "$(stat "${tp[$l]}")"; done
}

case ${1:-} in
  prepare) prepare ;;
  prepare_places) prepare_places ;;
  shapes) prepare_shapes ;;
  filters) prepare_filters ;;
  filter_matrix) shift; filter_matrix "$@" ;;
  build) shift; build "$@" ;;
  run) shift; run "$@" ;;
  clients) shift; clients "$@" ;;
  psql) shift; PGOPTIONS=$(options "${LABEL:-main}") exec psql -X "$@" "$ABDB" ;;
  stop) "$STEPS/cluster.sh" stop ;;
  *) sed -n '2,16p' "$0" >&2; exit 1 ;;
esac
