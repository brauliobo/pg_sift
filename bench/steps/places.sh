#!/usr/bin/env bash
# Multi-field autocomplete benchmark: sift against ParadeDB pg_search and rum on GeoNames places (name,
# alternate names, region, country, feature): 1-4 letter prefixes, whole words, 2-3 word ANDs, name
# matches boosted x3, top-k by score, filtered and unordered scans, count(*). rum (a tsvector column,
# `tsv @@ prefix query`, `<=>` ranking, in its own database so the column does not enlarge the others'
# table) has no per-field boost: its `boost` cells are NA. Engines whose extension is missing are skipped.
#
#   bench/steps/places.sh quick|full [run|cold]    cities15000 (34k places) or cities500 (236k places)
#
# Needs bench/bench setup (the engines, the places dataset). The tier cluster
# (bench/steps/cluster.sh) holds database `places`; every index is built once, then every round times every
# cell on every engine, the engine order rotating (single client, p50/p95 from the transaction
# latencies). Rounds stop once every cell's p50 spread is under TOL after MINR.
# Compact tables only: $DEST (default $RESULTS/places-<mode>-<time>)/{cells,builds,summary}.tsv and
# machine.txt. KEEP=1 keeps the cluster.
# Env: ENGINES (pg_search sift rum) CLASSES WLS SECS MINR MAXR TOL NQ (cold queries per class, not rum)
# TIMEOUT (statement_timeout of a cell, default 30s: slower cells are reported as error) RESULTS DEST.
set -euo pipefail
mode=${1:-quick}; what=${2:-run}
case $mode in
  quick) export TIER=Q; SECS=${SECS:-2} MINR=${MINR:-3} MAXR=${MAXR:-5}; CLASSES=${CLASSES:-p1 p3 w1 a2 a3 x2}; WLS=${WLS:-top boost filter count} ;;
  full)  export TIER=F; SECS=${SECS:-4} MINR=${MINR:-4} MAXR=${MAXR:-6}; CLASSES=${CLASSES:-p1 p2 p3 p4 w1 a2 a2s a3 x2}; WLS=${WLS:-top boost filter count scan} ;;
  *) sed -n '2,6p' "$0"; exit 1 ;;
esac
. "$(dirname "$0")/lib.sh"
TOL=${TOL:-0.2} NQ=${NQ:-12}
DATADIR=$DATA/places-$([[ $TIER == Q ]] && echo quick || echo full)
RESULTS=${RESULTS:-$BENCH/out/ab}
DEST=${DEST:-$RESULTS/places-$mode-$(date +%m%d-%H%M)}; mkdir -p "$DEST" "$ROOT/places"
export PGDATABASE=places
[[ -f $DATADIR/places.tsv ]] || { echo "no $DATADIR: bench/datasets/fetch.sh places" >&2; exit 1; }
cleanup() { [[ ${KEEP:-} ]] || { "$STEPS/cluster.sh" destroy >/dev/null 2>&1 || true; rm -rf "$ROOT/places"; }; }
trap cleanup EXIT

TEXT="place_search_text(name, alternate_names, admin1, country, country_code, feature)"
NAME="place_search_name(name, alternate_names)"
SECT="place_search_sections(name, alternate_names, admin1, country, country_code, feature)"
PGS_TEXT="($TEXT)::pdb.alias('ptext')"; PGS_NAME="($NAME)::pdb.alias('pname')"
declare -A INDEX=(
  [pg_search]="CREATE INDEX places_pg_search ON places USING bm25 (id, ($NAME::pdb.edge_ngram(1, 15, 'alias=pname')), ($TEXT::pdb.edge_ngram(1, 15, 'alias=ptext')))"
  [sift]="CREATE INDEX places_sift ON places USING sift (($SECT)) WITH (fields = 'name', prefix_index = 3)"
  [rum]="CREATE INDEX places_rum ON places USING rum (ptsv rum_tsvector_ops)")
declare -A SETUP=(
  [pg_search]="CREATE EXTENSION IF NOT EXISTS pg_search CASCADE"
  [sift]="CREATE EXTENSION IF NOT EXISTS pg_sift"
  [rum]="CREATE EXTENSION IF NOT EXISTS rum; ALTER TABLE places ADD COLUMN ptsv tsvector GENERATED ALWAYS AS (to_tsvector('simple', $TEXT)) STORED")
ENGINES=${ENGINES:-pg_search sift rum}
edb() { [[ $1 == rum ]] && echo places_rum || echo places; }   # rum's stored column lives in a copy of the table

# :q is what the user typed, :sq the same words as sift prefixes ("grand ho" -> "grand* ho*"), :sqb the boosted sift query,
# :tq the tsquery of rum ('grand':* & 'ho':*)
stmt() {
  case $1/$2 in
    top/pg_search)    echo "SELECT id FROM places WHERE $PGS_TEXT &&& :q ORDER BY pdb.score(id) DESC LIMIT 20" ;;
    top/sift)   echo "SELECT id FROM places WHERE ($SECT) @@> :sq ORDER BY sift.score(ctid) DESC LIMIT 20" ;;
    boost/pg_search)  echo "SELECT id FROM places WHERE $PGS_TEXT &&& :q AND (($PGS_NAME ||| :q::pdb.boost(3)) OR ($PGS_TEXT &&& :q)) ORDER BY pdb.score(id) DESC LIMIT 20" ;;
    boost/sift) echo "SELECT id FROM places WHERE ($SECT) @@> :sqb ORDER BY sift.score(ctid) DESC LIMIT 20" ;;
    filter/pg_search) echo "SELECT id FROM places WHERE $PGS_TEXT &&& :q AND country_code = 'US' ORDER BY pdb.score(id) DESC LIMIT 20" ;;
    filter/sift) echo "SELECT id FROM places WHERE ($SECT) @@> :sq AND country_code = 'US' ORDER BY sift.score(ctid) DESC LIMIT 20" ;;
    count/pg_search)  echo "SELECT count(*) FROM places WHERE $PGS_TEXT &&& :q" ;;
    count/sift) echo "SELECT count(*) FROM places WHERE ($SECT) @@> :sq" ;;
    scan/pg_search)   echo "SELECT id FROM places WHERE $PGS_TEXT &&& :q AND population >= 100000" ;;
    scan/sift)  echo "SELECT id FROM places WHERE ($SECT) @@> :sq AND population >= 100000" ;;
    top/rum)    echo "SELECT id FROM places WHERE ptsv @@ :tq::tsquery ORDER BY ptsv <=> :tq::tsquery LIMIT 20" ;;
    filter/rum) echo "SELECT id FROM places WHERE ptsv @@ :tq::tsquery AND country_code = 'US' ORDER BY ptsv <=> :tq::tsquery LIMIT 20" ;;
    count/rum)  echo "SELECT count(*) FROM places WHERE ptsv @@ :tq::tsquery" ;;
    scan/rum)   echo "SELECT id FROM places WHERE ptsv @@ :tq::tsquery AND population >= 100000" ;;
  esac
}
# the three query forms of the pq row number :i of a class
FORMS="quote_literal(q) AS q, quote_literal(sq) AS sq, quote_literal(format('(%s) AND (name:{%s}^3 OR (%s))', sq, sq, sq)) AS sqb,
       quote_literal(tq) AS tq
       FROM (SELECT q, regexp_replace(q, '(\S+)', '\1*', 'g') AS sq,
                    (SELECT string_agg(quote_literal(lexeme) || ':*', ' & ') FROM unnest(to_tsvector('simple', q))) AS tq FROM pq"

"$STEPS/cluster.sh" init >/dev/null; "$STEPS/cluster.sh" start >/dev/null
# sift errors beyond sift.max_expansions (512) terms; a typed prefix of a common word expands further on 236k places
drop_db places; drop_db places_rum
PGDATABASE=postgres q -c "CREATE DATABASE places" -c "ALTER DATABASE places SET sift.max_expansions = 100000"
ENGINES=$(available_engines "$ENGINES" | tr '\n' ' ')
[[ $ENGINES == *sift* ]] || { echo "sift is not installed: bench/steps/fetch.sh sift" >&2; exit 1; }
"$STEPS/machine.sh" >"$DEST/machine.txt"
q -v dir="$DATADIR" -f "$BENCH/sql/places_schema.sql"
q -c "ANALYZE places"
[[ $ENGINES != *rum* ]] || PGDATABASE=postgres q -c "CREATE DATABASE places_rum TEMPLATE places"
: >"$DEST/builds.tsv"
for e in $ENGINES; do
  db=$(edb "$e")
  heap0=$(PGDATABASE=$db q -c "SELECT pg_table_size('places')")
  t0=$(now_ms); PGDATABASE=$db q -c "${SETUP[$e]}"; t1=$(now_ms); PGDATABASE=$db q -c "${INDEX[$e]}"; t2=$(now_ms)
  PGDATABASE=$db q -c "VACUUM (ANALYZE) places" -c CHECKPOINT
  # engine, index build s, index bytes, table growth bytes (rum's column), setup s
  printf '%s\t%s\t%s\t%s\t%s\n' "$e" "$(elapsed_s "$t1" "$t2")" \
    "$(PGDATABASE=$db q -c "SELECT pg_relation_size('places_$e'::regclass)")" "$(( $(PGDATABASE=$db q -c "SELECT pg_table_size('places')") - heap0 ))" \
    "$(elapsed_s "$t0" "$t1")" >>"$DEST/builds.tsv"
done
q -c "CREATE EXTENSION IF NOT EXISTS pg_buffercache" >/dev/null
log "built: $(awk '{printf "%s %.1fs %.0fMB; ", $1, $2, $3 / 1048576}' "$DEST/builds.tsv")"

base() { q -c "SELECT min(id) - 1 FROM pq WHERE class = '$1'"; }

if [[ $what == cold ]]; then
  ENGINES=${ENGINES/rum/}
  printf 'workload\tclass\tengine\tp50_ms\tmax_ms\n' | tee "$DEST/cold.tsv"
  for wl in top boost filter count; do for c in p1 p2 a2 a3; do for e in $ENGINES; do
    b=$(base "$c")
    t=$(for i in $(seq "$NQ"); do
      s=$(stmt "$wl" "$e" | sed "s/:sqb/:'sqb'/; s/:sq\b/:'sq'/; s/:q\b/:'q'/g")
      IFS=$'\t' read -r tq tsq tsqb < <(q -F $'\t' -c "SELECT q, sq, format('(%s) AND (name:{%s}^3 OR (%s))', sq, sq, sq) FROM (SELECT q, regexp_replace(q, '(\S+)', '\1*', 'g') AS sq FROM pq WHERE id = $b + $i) x")
      evict_table places
      explain_ms "$s" -v q="$tq" -v sq="$tsq" -v sqb="$tsqb"
    done | median_max)
    printf '%s\t%s\t%s\t%s\n' "$wl" "$c" "$e" "$t" | tee -a "$DEST/cold.tsv"
  done; done; done
  exit 0
fi

cell() { # workload class engine -> "p50 p95" ms of single-client transactions: one primary-key lookup of the query text plus the statement
  local f=$ROOT/places/cell log=$ROOT/places/cell.log
  { printf '\\set i random(1, 300)\nSELECT %s WHERE id = %d + :i) x \\gset\n' "$FORMS" "$(base "$2")"; echo "$(stmt "$1" "$3");"; } >"$f.sql"
  rm -f "$log"*
  [[ -n $(stmt "$1" "$3") ]] || { echo "NA NA"; return; }   # the engine has no such query
  PGOPTIONS="-c statement_timeout=${TIMEOUT:-30s}" pgbench -n -f "$f.sql" -c 1 -T "$SECS" -l --log-prefix="$log" "$(edb "$3")" >/dev/null 2>&1 || { echo "nan nan"; return; }
  sort -n -k3,3 "$log"* | awk '{v[NR] = $3} END {printf "%.3f %.3f", v[int((NR + 1) / 2)] / 1000, v[int(NR * 0.95) < 1 ? 1 : int(NR * 0.95)] / 1000}'
  rm -f "$log"*
}
printf 'round\tload\tworkload\tclass\tengine\tp50\tp95\n' >"$DEST/cells.tsv"
stable() { spread_ok "$DEST/cells.tsv" 3,4,5 6 0.05; }   # workload, class, engine; p50 (cells under 0.05 ms are noise)
round() {
  local wl c e
  for wl in $WLS; do for c in $CLASSES; do for e in $(rotated "$1" $ENGINES); do
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$1" "$(loadavg)" "$wl" "$c" "$e" "$(cell "$wl" "$c" "$e" | tr ' ' '\t')" >>"$DEST/cells.tsv"
  done; done; done
}
rounds round
python3 -I - "$DEST" "$ENGINES" <<'PY'
import statistics as st, sys, collections
dest, engines = sys.argv[1], sys.argv[2].split()
rows = [l.rstrip('\n').split('\t') for l in open(dest + '/cells.tsv')][1:]
by = collections.defaultdict(lambda: ([], []))
for r in rows:
    by[r[2], r[3], r[4]][0].append(float(r[5]) if r[5] != 'NA' else None); by[r[2], r[3], r[4]][1].append(float(r[6]) if r[6] != 'NA' else None)
keys = sorted({(k[0], k[1]) for k in by}, key=lambda k: (list(dict.fromkeys(x[0] for x in by)).index(k[0]), k[1]))
loads = sorted({float(r[1]) for r in rows})
others = [e for e in engines if e != 'sift']
out = ['workload\tclass\t' + '\t'.join('%s_p50\t%s_p95' % (e, e) for e in engines) + '\t' + '\t'.join('%s/sift_p50' % e for e in others)]
def stat(e, wl, c):                              # (p50, p95): 'NA' no such query, 'error' a statement failed or timed out
    a = by[wl, c, e]
    if any(x is None for x in a[0]): return 'NA', 'NA'
    if any(x != x for x in a[0] + a[1]): return 'error', 'error'
    return st.median(a[0]), st.median(a[1])
for wl, c in keys:
    s = {e: stat(e, wl, c) for e in engines}
    f = lambda v: v if isinstance(v, str) else '%.2f' % v
    ratio = lambda e: 'n/a' if not all(isinstance(s[x][0], float) for x in (e, 'sift')) or not s['sift'][0] else '%.1fx' % (s[e][0] / s['sift'][0])
    out.append('%s\t%s\t' % (wl, c) + '\t'.join('%s\t%s' % (f(s[e][0]), f(s[e][1])) for e in engines) + '\t' + '\t'.join(ratio(e) for e in others))
out.append('rounds %d, load %.1f-%.1f' % (len({r[0] for r in rows}), loads[0], loads[-1]))
open(dest + '/summary.tsv', 'w').write('\n'.join(out) + '\n')
print('\n'.join(out))
PY
log "results in $DEST"
