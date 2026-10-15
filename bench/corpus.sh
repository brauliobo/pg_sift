#!/usr/bin/env bash
# Usage:
#   bench/corpus.sh export          # COPY document text out of source_db (read-only)
#   bench/corpus.sh csv FILE        # documents.tsv from a CSV export (title, comments, dim_a_id, ...), no database needed
#   TIER=S|M|L|Q|R bench/corpus.sh load     # build the tier's `bench` database and cache a dump
#   TIER=S|M|L bench/corpus.sh restore  # recreate `bench` from the cached dump
. "$(dirname "$0")/lib.sh"

declare -A LIMIT=([S]=300000 [M]=ALL [L]=ALL [Q]=ALL [R]=ALL)
# Q and R resample a small source (6,749 documents): Q about 200k documents, R about 1M
declare -A MULT=([S]=1 [M]=1 [L]=${L_MULT:-6} [Q]=${Q_MULT:-30} [R]=${R_MULT:-150})
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

csv() {
  mkdir -p "$DATA"
  python3 -I - "$1" "$SRC" <<'PY'
import csv, sys
csv.field_size_limit(10**9)
esc = lambda t: t.replace('\\', '\\\\').replace('\t', '\\t').replace('\n', '\\n').replace('\r', '')
n = 0
with open(sys.argv[2], 'w') as out:
    for row in csv.DictReader(open(sys.argv[1], encoding='utf-8')):
        body = '\n'.join(p for p in (row['title'].strip(), row['tags_a'].strip()) if p)
        if not body: continue
        n += 1
        out.write('\t'.join([str(n), esc(body), row['dim_a_id'] or '\\N', row['Num. Overall'] or '\\N',
                             (row['checker_origin'] or 'XX')[:2], row['created_date'] or '\\N', str(n % 97), esc('http://x/%d' % n)]) + '\n')
print(n, 'documents')
PY
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
  csv)     csv "${2:?CSV file}" ;;
  *) sed -n '2,6p' "$0" >&2; exit 1 ;;
esac
