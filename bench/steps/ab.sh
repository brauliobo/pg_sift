#!/usr/bin/env bash
# Interleaved A/B of sift builds on the slim corpus. One backend runs a class (or any pgbench script) while perf counts its user-space
# cycles and instructions per transaction: those hold up on a shared host where throughput does not. Builds are told apart by
# dynamic_library_path, one library per label; the on-disk format is the same, so one database serves them all.
#
#   bench/steps/ab.sh prepare                          cluster, slim corpus, index and the class scripts ($AB/pgb/CLASS.pgb)
#   bench/steps/ab.sh build LABEL [REF]                release library of git REF (default HEAD; "work": the working tree) -> $AB/libs/LABEL
#   bench/steps/ab.sh run CLASS REPS SECS LABEL...     cycles and instructions per transaction, one backend, mean +- standard error
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
ABDB=ab

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

run() { # CLASS REPS SECS LABEL...
  local name=$1 c=$(script "$1") reps=$2 secs=$3 r l pid cyc ins tps out
  shift 3
  declare -A cy in tp
  for r in $(seq "$reps"); do
    for l in "$@"; do
      out=$(mktemp)
      PGAPPNAME=ab PGOPTIONS=$(options "$l") pgbench -n -c 1 -j 1 -T $((secs + 3)) --random-seed=7 $(protocol "$c") ${PGB:-} -f "$c" "$ABDB" >"$out" 2>&1 &
      pid=$(backend ab)
      # past the connection and the first statements, then the window
      read -r cyc ins < <(perf stat -x, -e cycles:u,instructions:u -p "$pid" -- sleep "$secs" 2>&1 | awk -F, '/cycles/ {c = $1} /instructions/ {i = $1} END {print c, i}')
      wait
      tps=$(grep -oP 'tps = \K[0-9.]+(?= \(without)' "$out"); rm -f "$out"
      cy[$l]+="$(awk -v c="$cyc" -v t="$tps" -v s="$secs" 'BEGIN {printf "%.0f", c / (t * s)}') "
      in[$l]+="$(awk -v c="$ins" -v t="$tps" -v s="$secs" 'BEGIN {printf "%.0f", c / (t * s)}') "
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
  build) shift; build "$@" ;;
  run) shift; run "$@" ;;
  clients) shift; clients "$@" ;;
  psql) shift; PGOPTIONS=$(options "${LABEL:-main}") exec psql -X "$@" "$ABDB" ;;
  stop) "$STEPS/cluster.sh" stop ;;
  *) sed -n '2,16p' "$0" >&2; exit 1 ;;
esac
