#!/usr/bin/env bash
# sift for MariaDB, end to end: builds the UDF library, starts a private PostgreSQL (the pg_sift reference) and a private
# MariaDB, loads the 218,058-article Simple English Wikipedia corpus, then runs the correctness comparisons, the functional
# tests and the benchmark against InnoDB FULLTEXT, and removes both servers and all scratch files.
#
#   mariadb/test.sh                  everything
#   STEPS="build load" mariadb/test.sh   only some steps: build load pgref inno functional bench (in this order)
# Variables:
#   SIFT_BENCH_DATA  corpus and cache of bench/ (default /tmp/sift-bench): data/articles.tsv, cache/sift-F-ALL.dump
#   WORK             scratch root, deleted at the end unless KEEP=1 (default /tmp/sift-mdb-test)
#   PG_PORT          port of the private PostgreSQL, 56100-56169 (default 56166)
#   INNO_SUBSTRINGS  trace substrings (3 queries each) in the InnoDB set comparison (default 100; 1000 = the whole trace)
#   BENCH_QUERIES    queries per class in the benchmark (default 200; 1000 = the whole trace, about 1.5 hours: InnoDB takes
#                    0.1-0.8 s per ranked query)
#   BUFFER_POOL      innodb_buffer_pool_size (default 3G)
# Needs: PostgreSQL 17+/18 with server headers, MariaDB server + headers, libmariadb, python3. No sudo.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(dirname "$HERE")
DATA=${SIFT_BENCH_DATA:-/tmp/sift-bench}
WORK=${WORK:-/tmp/sift-mdb-test}
STEPS=${STEPS:-build load pgref inno functional bench}
export SIFT_MDB_ROOT=$WORK/mariadb SIFT_BUILD=$WORK/build SIFT_TMP=$WORK/pg SIFT_PORT=${PG_PORT:-56166}
export PGHOST=$SIFT_TMP/sock PGPORT=$SIFT_PORT PGUSER=postgres
SRV=$HERE/server.sh
MDBQ=$SIFT_BUILD/mdbq
T=$WORK/t
sql() { "$SRV" sql -N -B bench "$@"; }
mq() { "$MDBQ" "$SIFT_MDB_ROOT/mysql.sock" bench "$@"; }
step() { [[ " $STEPS " == *" $1 "* ]]; }
say() { printf '\n== %s\n' "$*"; }
secs() { awk -v a="$1" -v b="$(date +%s.%N)" 'BEGIN { printf "%.1f s", b - a }'; }

cleanup() {
  "$SRV" stop 2>/dev/null || true
  "$REPO/scripts/pgtest.sh" stop >/dev/null 2>&1 || true
  [[ -n ${KEEP:-} ]] || rm -rf "$WORK"
}
trap cleanup EXIT
mkdir -p "$T"

build() {
  say "build"
  nice make -C "$HERE" -j4 OUT="$SIFT_BUILD" >"$WORK/build.log" 2>&1 || { tail -20 "$WORK/build.log"; exit 1; }
  ls -l "$SIFT_BUILD/libsift_udf.so"
}

start_pg() {
  [[ -d $SIFT_TMP/root ]] || SIFT_TESTING= nice "$REPO/scripts/pgtest.sh" install >"$WORK/pg-install.log" 2>&1
  SIFT_TESTING= "$REPO/scripts/pgtest.sh" start >>"$WORK/pg-install.log" 2>&1
  psql -X -At -d bench -c 'select 1' >/dev/null 2>&1 && return
  createdb bench
  nice pg_restore --no-owner -j2 -d bench "$DATA/cache/sift-F-ALL.dump"
  psql -X -d bench -c "CREATE EXTENSION pg_sift" -c "CREATE INDEX docs_fts ON docs USING sift (body)"
}

load() {
  say "load: MariaDB server, table docs"
  "$SRV" start
  "$SRV" sql -e "CREATE DATABASE bench DEFAULT CHARACTER SET utf8mb4"
  "$SRV" sql bench <"$HERE/sql/functions.sql"
  "$SRV" sql bench <"$HERE/sql/sift.sql"
  cut -f1,2 "$DATA/data/articles.tsv" >"$T/docs.tsv"
  sql -e "CREATE TABLE docs (id BIGINT PRIMARY KEY, body MEDIUMTEXT NOT NULL) ENGINE=InnoDB"
  local t0; t0=$(date +%s.%N)
  sql -e "LOAD DATA INFILE '$T/docs.tsv' INTO TABLE docs CHARACTER SET utf8mb4 (id, body)"
  echo "table load: $(secs "$t0"), rows $(sql -e 'SELECT COUNT(*) FROM docs')"
  rm -f "$T/docs.tsv"

  say "build: InnoDB FULLTEXT index"
  t0=$(date +%s.%N)
  sql -e "ALTER TABLE docs ADD FULLTEXT INDEX ft_body (body)"
  echo "InnoDB FULLTEXT build: $(secs "$t0")"
  echo "InnoDB FULLTEXT size: $(du -cb "$SIFT_MDB_ROOT"/data/bench/FTS_*.ibd | tail -1 | cut -f1) bytes (FTS_*.ibd; docs.ibd, the table, is $(stat -c %s "$SIFT_MDB_ROOT/data/bench/docs.ibd") bytes)"

  say "build: sift index (queue the table, drain into segments)"
  t0=$(date +%s.%N)
  sql -e "CALL sift_attach('wiki', 'docs', 'id', 't.body', 32)" >/dev/null
  echo "attach (create, triggers, queue 218058 rows): $(secs "$t0")"
  t0=$(date +%s.%N)
  sql -e "CALL sift_sync('wiki')" >/dev/null
  echo "drain (build): $(secs "$t0")"
  sql -e "SELECT sift_stats('wiki')"
  echo "sift size: $(du -sb "$SIFT_MDB_ROOT/data/.sift/wiki" | cut -f1) bytes"
  echo "a second index without the freshness features, to measure what they cost:"
  sql -e "CALL sift_attach_opts('wiki_plain', 'docs', 'id', 't.body', '{\"dpp\":32,\"fresh\":false,\"recheck\":false}'); CALL sift_sync('wiki_plain')" >/dev/null
}

# trace and PostgreSQL reference
pgref() {
  say "correctness: pg_sift (PostgreSQL) vs sift for MariaDB, same corpus and queries"
  start_pg
  psql -X -A -t -d bench -c "COPY (SELECT id, mode, array_to_string(terms,' ') FROM queries ORDER BY id) TO STDOUT" >"$T/trace.tsv"
  python3 -I "$HERE/tools/genq.py" "$T/trace.tsv" --regex >"$T/queries.tsv"
  TIE=1 nice "$HERE/tools/pg_ref.sh" "$T/queries.tsv" >"$T/ref.tsv"
  python3 -I "$HERE/bench/mkstmts.py" sift-ref "$T/queries.tsv" >"$T/ref-stmts.sql"
  mq print "$T/ref-stmts.sql" >"$T/ref-out.txt"
  python3 -I "$HERE/tools/json2ref.py" "$T/ref-stmts.sql" "$T/ref-out.txt" >"$T/got.tsv"
  echo "queries: $(wc -l <"$T/queries.tsv") (trace of $(wc -l <"$T/trace.tsv") and/or/phrase plus prefix, wildcard, fuzzy, NOT, boost, group, nested, phrases under OR/NOT, regular expressions)"
  python3 -I "$HERE/tools/compare.py" "$T/ref.tsv" "$T/got.tsv" --label "pg_sift vs sift/MariaDB (count, top-10 order, float32 scores)"
}

# match sets against InnoDB FULLTEXT: the table as it is, then with the characters InnoDB handles differently turned into
# spaces (underscore, apostrophes, soft hyphen), which isolates the analyzer differences
inno() {
  say "differential: sift vs InnoDB FULLTEXT match sets (boolean mode, stopwords off, min token size 1)"
  [[ -f $T/trace.tsv ]] || psql -X -A -t -d bench -c "COPY (SELECT id, mode, array_to_string(terms,' ') FROM queries ORDER BY id) TO STDOUT" >"$T/trace.tsv"
  head -n "$((3 * ${INNO_SUBSTRINGS:-100}))" "$T/trace.tsv" >"$T/sub.tsv"
  python3 -I "$HERE/bench/mkstmts.py" sift-matches "$T/sub.tsv" >"$T/s.sql"
  python3 -I "$HERE/bench/mkstmts.py" inno-matches "$T/sub.tsv" >"$T/i.sql"
  mq print "$T/s.sql" >"$T/s.out"
  mq print "$T/i.sql" >"$T/i.out"
  echo "--- InnoDB on the text as stored"
  python3 -I "$HERE/tools/diff_inno.py" "$T/sub.tsv" "$T/s.out" "$T/i.out"
  sql -e "CREATE TABLE docs_norm (id BIGINT PRIMARY KEY, body MEDIUMTEXT NOT NULL) ENGINE=InnoDB;
    INSERT INTO docs_norm SELECT id, REPLACE(REPLACE(REPLACE(REPLACE(body, '_', ' '), '''', ' '), '’', ' '), CONVERT(UNHEX('C2AD') USING utf8mb4), ' ') FROM docs;
    ALTER TABLE docs_norm ADD FULLTEXT INDEX ft_body (body)"
  python3 -I "$HERE/bench/mkstmts.py" inno-matches "$T/sub.tsv" --table=docs_norm >"$T/i2.sql"
  mq print "$T/i2.sql" >"$T/i2.out"
  echo "--- InnoDB on the text with _ ' U+2019 U+00AD replaced by spaces"
  python3 -I "$HERE/tools/diff_inno.py" "$T/sub.tsv" "$T/s.out" "$T/i2.out"
  sql -e "DROP TABLE docs_norm"
}

# the checks on small tables, in database ft; the corpus server is restarted by some of them (version, live)
functional() {
  say "functional tests (database ft)"
  local t failed=0
  for t in functional fresh keys filters options version live stress regex_check repl; do
    "$HERE/tests/$t.sh" || failed=1
  done
  [[ $failed == 0 ]] || { echo "some functional checks FAILED"; exit 1; }
}

bench() {
  say "benchmark: one client, warm cache (median in microseconds, queries per second of the second pass)"
  [[ -f $T/trace.tsv ]] || psql -X -A -t -d bench -c "COPY (SELECT id, mode, array_to_string(terms,' ') FROM queries ORDER BY id) TO STDOUT" >"$T/trace.tsv"
  "$SRV" restart
  sql -e "SELECT COUNT(*) FROM docs" >/dev/null
  QUERIES=${BENCH_QUERIES:-200} MDBQ=$MDBQ "$HERE/bench/run.sh" "$T/trace.tsv"
}

echo "load average $(cut -d' ' -f1-3 /proc/loadavg)"
step build && build
step load && load
step pgref && pgref
step inno && inno
step functional && functional
step bench && bench
echo; echo "done"
