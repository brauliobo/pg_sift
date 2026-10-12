#!/usr/bin/env bash
# Usage:
#   bench/corpus.sh export          # COPY document text out of source_db (read-only)
#   TIER=S|M|L bench/corpus.sh load     # build the tier's `bench` database and cache a dump
#   TIER=S|M|L bench/corpus.sh restore  # recreate `bench` from the cached dump
. "$(dirname "$0")/lib.sh"

declare -A LIMIT=([S]=300000 [M]=ALL [L]=ALL)
declare -A MULT=([S]=1 [M]=1 [L]=${L_MULT:-6})
SRC=$DATA/documents.tsv
DUMP=$CACHE/sift-$TIER.dump

export_src() {
  mkdir -p "$DATA"
  nice psql -X -v ON_ERROR_STOP=1 -q -d source_db -f "$BENCH/sql/export.sql" >"$SRC.tmp"
  mv "$SRC.tmp" "$SRC"
  log "exported $(wc -l <"$SRC") documents to $SRC"
}

fresh_db() {
  "$BENCH/cluster.sh" init
  "$BENCH/cluster.sh" start
  PGDATABASE=postgres q -c "DROP DATABASE IF EXISTS bench WITH (FORCE)" -c "CREATE DATABASE bench"
}

load() {
  fresh_db
  q -f "$BENCH/sql/schema.sql"
  q -c "\\copy src FROM '$SRC'"
  nice psql -X -q -v limit="${LIMIT[$TIER]}" -v mult="${MULT[$TIER]}" -v nsub="${NSUB:-1000}" -f "$BENCH/sql/load.sql"
  mkdir -p "$CACHE"
  pg_dump -Fc -Z zstd:3 -f "$DUMP" bench
  log "bench: $(q -c "SELECT count(*) || ' docs, ' || pg_size_pretty(pg_database_size('bench')) FROM docs") -> $DUMP"
}

restore() {
  fresh_db
  pg_restore -j 4 -d bench "$DUMP"
  q -c "VACUUM (ANALYZE) docs" -c CHECKPOINT
  log "restored $DUMP"
}

case ${1:-} in
  export)  export_src ;;
  load)    load ;;
  restore) restore ;;
  *) sed -n '2,5p' "$0" >&2; exit 1 ;;
esac
