#!/usr/bin/env bash
# Index options against pg_sift with the same options: for each option set the same table is indexed in a private PostgreSQL
# (pg_sift) and in MariaDB (sift), and a battery of queries must give the same count and the same top-10 order.
# Needs the libpq environment of a PostgreSQL with pg_sift (PGHOST, PGPORT, PGUSER; mariadb/test.sh sets it up) and the
# private MariaDB server of mariadb/server.sh.
#   mariadb/tests/options.sh
set -uo pipefail
. "$(dirname "$0")/lib.sh"
NAME=options
PGDB=${PGDB:-opt}
pg() { psql -X -A -t -d "$PGDB" -c "$1"; }

TMP=$(mktemp -d /tmp/sift-mdb-opt.XXXXXX)
trap 'rm -rf "$TMP"' EXIT
python3 -I - >"$TMP/opt.tsv" <<'E'
words = ['Alpha', 'alphabet', 'beta', 'bets', 'Über', 'zèbre', 'gamma', 'running', 'runs', 'ran', 'Résumé', 'resume',
         'cafés', 'cafe', 'naïve', 'naive', 'delta', 'e-mail', 'x1', 'x2', 'quick', 'quickly', 'Quick']
titles = ['Alpha Beta', 'alpha beta', 'Über Zèbre', 'Résumé', 'resume', 'Running', 'gamma', 'Quick Quickly']
s = 12345
for i in range(1, 401):
    ws = []
    for _ in range(7):
        s = (s * 1103515245 + 12345) % 2147483648
        ws.append(words[(s >> 8) % len(words)])
    s = (s * 1103515245 + 12345) % 2147483648
    print(f"{i}\t{titles[(s >> 8) % len(titles)]}\t{' '.join(ws)}")
E

dropdb --if-exists "$PGDB" >/dev/null 2>&1; createdb "$PGDB" || { echo "FAIL no PostgreSQL (set PGHOST, PGPORT, PGUSER)"; exit 1; }
pg "CREATE EXTENSION pg_sift; CREATE FUNCTION sections(a text, b text) RETURNS text LANGUAGE sql IMMUTABLE AS \$\$ SELECT a || chr(29) || b \$\$" >/dev/null
fresh_db o1 o2 o3 o4 o5 o6 o7 o8

# case: name | PostgreSQL column or expression | PostgreSQL WITH options | MariaDB text expression | MariaDB options JSON
cases=(
  "o1|body|k1 = 1.2, b = 0.75|t.body|{}"
  "o2|body|k1 = 2.0, b = 0.3|t.body|{\"k1\":2.0,\"b\":0.3}"
  "o3|body|stemmer = en|t.body|{\"stemmer\":\"en\"}"
  "o4|body|case_folding = false, accent_folding = false|t.body|{\"case_folding\":false,\"accent_folding\":false}"
  "o5|body|positions = off|t.body|{\"positions\":false}"
  "o6|body|prefix_index = 2|t.body|{\"prefix_index\":2}"
  "o7|sections(title, body)|fields = 'title,body'|t.body|{\"fields\":[{\"name\":\"title\",\"expr\":\"t.title\"},{\"name\":\"body\",\"expr\":\"t.body\"}]}"
  "o8|title|analyzer = keyword|t.title|{\"analyzer\":\"keyword\"}"
)
queries=('alpha' 'Alpha' 'alp*' 'running' 'run*' 'resume' 'ube*' 'zebre' 'alpha AND beta' 'alpha OR gamma'
         'alpha AND NOT beta' '"alpha beta"' '"quick quickly"' 'beta^3 OR gamma' 'naive' 'caf*' 'quick* AND NOT quickly'
         '/al.*a/' 'alpha~1')
fq=('title:alpha' 'title:beta OR body:gamma' 'body:beta^3 OR title:alpha' 'title:(alpha beta)' 'title:alp*' 'nosuch' 'alpha')
kq=('Alpha Beta' 'alpha beta' 'Resume' 'resume' 'gamma' 'Running' 'quick quickly' 'Über Zèbre' 'uber zebre')

for c in "${cases[@]}"; do
  IFS='|' read -r name pexpr popt mexpr mopt <<<"$c"
  pg "CREATE TABLE $name (id int PRIMARY KEY, title text, body text); CREATE INDEX ${name}_i ON $name USING sift ($pexpr) WITH ($popt)" >/dev/null
  psql -X -q -d "$PGDB" -c "\\copy $name FROM '$TMP/opt.tsv'" >/dev/null
  sql "CREATE TABLE $name (id BIGINT PRIMARY KEY, title VARCHAR(100), body TEXT)"
  sql "LOAD DATA INFILE '$TMP/opt.tsv' INTO TABLE $name CHARACTER SET utf8mb4"
  sql "CALL sift_attach_opts('$name', '$name', 'id', '$mexpr', '$mopt')" >/dev/null
  [[ $name == o3 ]] || sync "$name"            # o3 stays in the queue: the same answers before the drain
  qs=("${queries[@]}"); [[ $name == o7 ]] && qs=("${fq[@]}"); [[ $name == o8 ]] && qs=("${kq[@]}")
  same=0 diff=0
  for q in "${qs[@]}"; do
    qp=${q//\'/\'\'}
    want=$(pg "SELECT count(*) || ' ' || coalesce((SELECT string_agg(id::text, ',' ORDER BY s DESC, id) FROM (SELECT id, sift.score(ctid) s FROM $name WHERE $pexpr @@> '$qp' ORDER BY s DESC, id LIMIT 10) x), '') FROM $name WHERE $pexpr @@> '$qp'" 2>&1)
    got="$(cnt "$q" "$name") $(top "$q" 10 "$name")"
    got=${got% }; want=${want% }
    if [[ $want == "$got" ]]; then same=$((same + 1)); else diff=$((diff + 1)); echo "  differs [$name] $q: pg [$want] mdb [$got]"; fi
  done
  check "options $name ($popt): $same of ${#qs[@]} queries equal to pg_sift" 0 "$diff"
done

# the options the engine reports back, and the ones it refuses
check "options: the configuration is kept in the index" 1 "$(sql "SELECT cfg LIKE '%prefix_index=2%' FROM sift_indexes WHERE name = 'o6'")"
check_match "options: an unknown stemmer is refused" "stemmer" "$(sqle "CALL sift_attach_opts('bad', 'o1', 'id', 't.body', '{\"stemmer\":\"klingon\"}')")"
check_match "options: keyword with a stemmer is refused" "keyword" "$(sqle "CALL sift_attach_opts('bad', 'o1', 'id', 't.body', '{\"analyzer\":\"keyword\",\"stemmer\":\"en\"}')")"
check "options: a refused attach leaves nothing behind" 0 "$(sql "SELECT COUNT(*) FROM sift_indexes WHERE name = 'bad'")"
pg "SELECT 1" >/dev/null; dropdb "$PGDB" >/dev/null 2>&1
finish
