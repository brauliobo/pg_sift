#!/usr/bin/env bash
# Usage:
#   bench/steps/corpus.sh fetch               # download and convert the public datasets (bench/datasets/fetch.sh articles)
#   TIER=Q|F bench/steps/corpus.sh load       # build the tier's `bench` database from $DATA/articles.tsv and cache a dump
#   TIER=Q|F bench/steps/corpus.sh restore    # recreate `bench` from the cached dump
#   TIER=Q|F bench/steps/corpus.sh ensure     # start the cluster and make sure `bench` exists (load, then restore)
# Tiers: Q (quick) 150,000 articles (Q_DOCS), F (full) all 218k Simple English Wikipedia articles.
. "$(dirname "$0")/lib.sh"

SRC=$DATA/articles.tsv

fresh_db() {
  "$STEPS/cluster.sh" init
  "$STEPS/cluster.sh" start
  drop_db bench
  PGDATABASE=postgres q -c "CREATE DATABASE bench"
}

load() {
  [[ -f $SRC ]] || { echo "no $SRC: run bench/steps/corpus.sh fetch" >&2; exit 1; }
  fresh_db
  q -f "$BENCH/sql/schema.sql"
  q -c "\\copy src FROM '$SRC'"
  nice psql -X -q -v limit="${TIER_DOCS[$TIER]}" -v nsub="${NSUB:-1000}" -f "$BENCH/sql/load.sql"
  mkdir -p "$CACHE"
  pg_dump -Fc -Z zstd:3 -f "$DUMP" bench
  log "bench: $(q -c "SELECT count(*) || ' docs, ' || pg_size_pretty(pg_database_size('bench')) FROM docs") -> $DUMP"
}

restore() {
  fresh_db
  pg_restore -j "${RESTORE_JOBS:-${WORKERS:-4}}" -d bench "$DUMP"
  q -c "VACUUM (ANALYZE) docs" -c CHECKPOINT
  log "restored $DUMP"
}

ensure() {
  "$STEPS/cluster.sh" init
  "$STEPS/cluster.sh" start
  db_exists bench || { [[ -f $DUMP ]] || load; restore; }
}

case ${1:-} in
  fetch)   "$BENCH/datasets/fetch.sh" articles ;;
  load)    load ;;
  restore) restore ;;
  ensure)  ensure ;;
  *) sed -n '2,6p' "$0" >&2; exit 1 ;;
esac
