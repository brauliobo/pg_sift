#!/usr/bin/env bash
# What CREATE INDEX, REINDEX and REINDEX CONCURRENTLY of a sift_col index print: one summary NOTICE of a few lines, not one NOTICE with a DETAIL and a HINT for each choice. The index
# of the test gets order arrays, key lists, key_cut, array lengths and track_changes (the 100,000 rows the order arrays ask for). The test counts the lines of each statement against
# the limit of the table below, and proves that everything the summary names resolves: every name is a row of sift.index_choices(index), every row has its row of sift.help(slug), the
# count of the first line is the number of automatic choices of the function, and client_min_messages = warning hides the summary.
. "$(dirname "$0")/lib.bash"

REPO=$(cd "$(dirname "$0")/../.." && pwd)
SOCK=$(dir ch)/sock
# psql FLAGS: the node, notices on (messages of every level go to the output)
p() { PGOPTIONS="-c client_min_messages=notice" psql -X -q -h "$SOCK" -p "$(port ch)" -U postgres -d postgres "$@" 2>&1; }
# s STATEMENT...: the output of the statements, tuples only
s() { p -At "${@/#/--command=}"; }

# statement|most lines it may print|most DETAIL and HINT lines: the summary (a DETAIL and a HINT), the format notice (a HINT); the concurrent build adds the HINT of the log
STATEMENTS=(
	"CREATE INDEX facts_sift_col ON facts USING sift_col (id, tenant_id, foreign_id, foreign_id_b, event_date, value_num, tags_a, updated_at)|8|3"
	"REINDEX INDEX facts_sift_col|8|3"
	"REINDEX INDEX CONCURRENTLY facts_sift_col|10|4"
)

plan 21
node_init ch 74
node_start ch || bail "start failed"
s "CREATE EXTENSION pg_sift" \
	"CREATE TABLE facts (id bigint PRIMARY KEY, tenant_id int, foreign_id int, foreign_id_b int, event_date date, value_num numeric(12,2), tags_a int[], updated_at timestamptz)" \
	"INSERT INTO facts SELECT i, i % 162, i % 40, (i * 7919) % 5000, date '2024-01-01' + (i * 31 % 700), (i * 37 % 100000) / 100.0, ARRAY[i % 10, i % 7], now() - i * interval '1 minute' FROM generate_series(1, 100000) i" \
	"ANALYZE facts" >/dev/null || bail "setup failed"

for st in "${STATEMENTS[@]}"; do
	IFS='|' read -r sql max details <<<"$st"
	out=$(s "$sql")
	lines=$(wc -l <<<"$out")
	check "$sql: $lines lines, at most $max" test "$lines" -le "$max"
	check "$sql: one summary NOTICE" test "$(grep -c 'automatic choices\?' <<<"$out")" == 1
	check "$sql: no DETAIL or HINT for each choice ($details at most)" test "$(grep -c -E '^(DETAIL|HINT):' <<<"$out")" -le "$details"
	if [[ $sql == *CONCURRENTLY* ]]; then
		check "the concurrent build says in one line that the log is not complete and the command that completes it" grep -q -E '^NOTICE:  sift_col index "facts_sift_col": track_changes log is not complete after a concurrent build$' <<<"$out"
		check "... and its hint is one line with the command" grep -q -E '^HINT:  The next VACUUM completes the log; REINDEX INDEX facts_sift_col without CONCURRENTLY does it now\.$' <<<"$out"
	fi
	summary=$out
done

# what the last summary names: DETAIL entries "name = value" (advice: and limit: first), separated by semicolons, on at most two lines
names=$(sed -n '/^DETAIL:/,/^HINT:/p' <<<"$summary" | sed -e '1s/^DETAIL:  //' -e '/^HINT:/d' | tr ';' '\n' | sed -E -e 's/^ *(advice: |limit: )?//' -e 's/ = .*//' -e '/^and [0-9]+ more$/d' -e '/^$/d')
choices=$(s "SELECT choice FROM sift.index_choices('facts_sift_col')")
check "the summary names at least 4 choices" test "$(wc -l <<<"$names")" -ge 4
check "every name of the summary is a row of sift.index_choices" test -z "$(comm -23 <(sort -u <<<"$names") <(sort -u <<<"$choices"))"
n_auto=$(s "SELECT count(*) FROM sift.index_choices('facts_sift_col') WHERE kind = 'auto'")
check "the count of the first line is the automatic choices of sift.index_choices" grep -q -E ": $n_auto automatic choices?(,| \()" <<<"$summary"
check "every row of sift.index_choices has a reason, a command and a row of sift.help" test "$(s "SELECT count(*) FROM sift.index_choices('facts_sift_col') c WHERE c.why IS NULL OR c.off IS NULL OR NOT EXISTS (SELECT 1 FROM sift.help(c.slug))")" == 0
check "the order arrays of the index show their bytes a row in sift.index_choices" test "$(s "SELECT count(*) FROM sift.index_choices('facts_sift_col') WHERE choice = 'order_array_columns' AND cost ~ 'bytes a row'")" == 1
check "sift.help() lists every row of docs/messages.tsv" test "$(s "SELECT count(*) FROM sift.help()")" == "$(grep -c -v -E '^(#|$)' "$REPO/docs/messages.tsv")"
check "sift.help(slug) of an unknown slug is an error that points to the list" grep -q 'SELECT id FROM sift.help' <<<"$(s "SELECT * FROM sift.help('no-such-slug')")"
check "the format line names the column format minor of the order arrays" grep -q -E '^NOTICE:  sift_col index "facts_sift_col" is column format minor [0-9]+, which a library before [0-9.]+ cannot read$' <<<"$summary"
check "client_min_messages = warning hides the summary" test -z "$(PGOPTIONS='-c client_min_messages=warning' psql -X -q -h "$SOCK" -p "$(port ch)" -U postgres -d postgres -c 'REINDEX INDEX facts_sift_col' 2>&1)"
check "a plain b-tree is refused with its name" grep -q 'is not a sift_col index' <<<"$(s "SELECT * FROM sift.index_choices('facts_pkey')")"
finish
