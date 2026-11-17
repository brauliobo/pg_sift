#!/usr/bin/env bash
# The sample of the automatic key_cut on the 8-page-superblock flavour of scripts/scaling-gate.sh (units of 8 pages, 2,328 rows at most), where a heap of 116 superblocks has 928 rows:
#  - the first look of a table of 116 superblocks sampled 7 of the 8 it plans and ended at "fewer than 16 superblocks" when a planned superblock held no row (the empty pages a
#    bulk load leaves at the end, a deleted and vacuumed range): the same count of superblocks and the same stride of the stretches, at 928 rows and not 4,000,000. A superblock with
#    no row gives its place to the next that has rows, and the stretches past the end of the heap take the last ones that have rows;
#  - the bytes of the memory context of the decision (the scope col_key_cut_decision, sift._test_mem_highwater) at 13 sizes of sample (16 to 128 sampled superblocks, 3 and 4 leaves) are below the bound of the code, whatever the rows of the table: 320 kB and 20 bytes for each row of a unit, and for each sampled row 84 bytes (the address, the
#    lists and the sort of the model) and 10 for each leaf and 32 for each key candidate (the cells of its keys, which double);
#  - a budget of a ten-thousandth of the budget's makes the decision fail by name: the checkpoints measure the context that holds the sample.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)

root=$("$REPO/scripts/scaling-gate.sh" build 2>/dev/null)
rc=$?
if ((rc == 77)); then echo '1..0 # SKIP the 8-page flavour needs PostgreSQL 18'; exit 0; fi
((rc == 0)) || { echo '1..1'; echo 'not ok 1 - build of the 8-page flavour'; exit 1; }

node_init ks 34 "autovacuum = off
sift.col_auto_merge = off
shared_buffers = 128MB
max_wal_size = 2GB
extension_control_path = '$root$(pg_config --sharedir):\$system'
dynamic_library_path = '$root$(pg_config --pkglibdir):\$libdir'"
node_start ks || bail "start failed"
sql ks "CREATE EXTENSION pg_sift_test CASCADE" >/dev/null || bail "extension failed"

# kx(sbs, pad): a table of sbs superblocks of 8 pages (the rows a page holds measured on 4,000 rows of its width); kxi(leaves): its index of 2 to 4 leaves
sql ks "
CREATE FUNCTION kx(sbs int, pad int) RETURNS void LANGUAGE plpgsql AS \$f\$
DECLARE per_page int;
BEGIN
	DROP TABLE IF EXISTS kx_pilot, kx;
	CREATE TABLE kx_pilot AS SELECT g AS id, (g::bigint * 7919) % 2003 AS foreign_id, g % 7 AS status, date '2020-01-01' + (g * 31 % 1500) AS d, (g * 37) % 9973 AS a, repeat('x', pad) AS p FROM generate_series(1, 4000) g;
	SELECT 4000 / (max((ctid::text::point)[0]::int) + 1) INTO per_page FROM kx_pilot;
	CREATE TABLE kx WITH (autovacuum_enabled = false) AS SELECT g AS id, (g::bigint * 7919) % 2003 AS foreign_id, g % 7 AS status, date '2020-01-01' + (g * 31 % 1500) AS d, (g * 37) % 9973 AS a, repeat('x', pad) AS p FROM generate_series(1, sbs * 8 * per_page) g;
END \$f\$;
CREATE FUNCTION kxi(leaves int) RETURNS void LANGUAGE plpgsql AS \$f\$
BEGIN
	EXECUTE format('CREATE INDEX kx_i ON kx USING sift_col (%s) WITH (key_cut = ''off'')', (SELECT string_agg(c, ', ') FROM unnest((ARRAY['foreign_id', 'status', 'd', 'a'])[1:leaves]) c));
END \$f\$;
-- the decision of kx_i and the bytes its memory context held at the most
CREATE FUNCTION kd(leaves int) RETURNS TABLE (sampled int, ok_bound bool, peak int8, bound int8) LANGUAGE plpgsql AS \$f\$
DECLARE s int; p int8; per_sb int;
BEGIN
	PERFORM * FROM sift._test_mem_highwater(true);
	SELECT d.sampled INTO s FROM sift.key_cut_decision('kx_i') d;
	SELECT h.peak INTO p FROM sift._test_mem_highwater(true) h WHERE h.scope = 'col_key_cut_decision';
	SELECT max(n) INTO per_sb FROM (SELECT count(*) AS n FROM kx GROUP BY (ctid::text::point)[0]::int / 8) c;
	RETURN QUERY SELECT s, p <= 327680 + 20 * 2328 + s::int8 * per_sb * (84 + 10 * leaves + 32), p, 327680 + 20 * 2328 + s::int8 * per_sb * (84 + 10 * leaves + 32);
END \$f\$;" >/dev/null || bail "functions failed"

plan 19
# the edge case: 116 superblocks of 928 rows of a page each, the first look plans 8 of them (stretches of 14.5 superblocks)
sql ks "CREATE TABLE e116 (id bigint, foreign_id int, d date, pad text) WITH (fillfactor = 10, autovacuum_enabled = false)" \
	"INSERT INTO e116 SELECT i, (i * 7919) % 211, date '2020-01-01' + (i * 31 % 1500), repeat('x', 500) FROM generate_series(1, 928) i" \
	"CREATE INDEX e116_i ON e116 USING sift_col (foreign_id, d) WITH (key_cut = 'off')" >/dev/null || bail "e116 failed"
dec() { sql ks "SELECT sampled || ' ' || (reason NOT LIKE 'the sample holds%')::int FROM sift.key_cut_decision('$1')"; }
out=$(sql ks "SELECT ceil(pg_relation_size('e116') / 8192 / 8.0), count(*) FROM e116" | tr '|' ' ')
check "the table has 116 superblocks of 8 pages and 928 rows" test "$out" == "116 928"
out=$(dec e116_i); check "all 116 superblocks hold rows: the first look reads its 8 and decides" test "$out" == "8 1"
sql ks "DELETE FROM e116 WHERE id > 920" "VACUUM (TRUNCATE false) e116" >/dev/null || bail "delete failed"
out=$(dec e116_i); check "the last superblock empty (planned: the 116th): 8 sampled, decided (7 and skipped before)" test "$out" == "8 1"
sql ks "DELETE FROM e116 WHERE id > 888" "VACUUM (TRUNCATE false) e116" >/dev/null || bail "delete failed"
out=$(dec e116_i); check "the last 5 superblocks empty: the stretches that find none take the last that have rows" test "$out" == "8 1"
sql ks "DELETE FROM e116 WHERE id <= 232" "VACUUM (TRUNCATE false) e116" >/dev/null || bail "delete failed"
out=$(dec e116_i); check "the first 29 superblocks empty too: the first stretches take the next that have rows" test "$out" == "8 1"
sql ks "DROP TABLE e116" >/dev/null

# the exact memory of the decision
for c in "64 0 3" "64 20 3" "64 100 3" "128 0 3" "128 20 3" "128 60 3" "128 100 3" "128 250 3" "256 250 3" "512 250 3" "256 0 3" "64 100 4" "128 150 4"; do
	set -- $c
	sql ks "SELECT kx($1, $2)" "VACUUM (FREEZE, ANALYZE) kx" "SELECT kxi($3)" >/dev/null || bail "fixture $c failed"
	out=$(sql ks "SELECT * FROM kd($3)" | tr '|' ' ')
	read -r sampled within peak bound <<<"$out"
	check "the decision of $1 superblocks, $2 bytes of padding, $3 leaves sampled $sampled superblocks and held $peak bytes, no more than its bound of $bound" test "$within-$((sampled >= 16))" == t-1
done
sql ks "SELECT kx(64, 150)" "VACUUM (FREEZE, ANALYZE) kx" "SELECT kxi(3)" >/dev/null
out=$(sql ks "SET sift.test_mem_budget_scale = 0.0001" "SELECT * FROM sift.key_cut_decision('kx_i')" 2>&1)
check "a budget of a ten-thousandth fails the decision by name" grep -q 'sift memory budget exceeded: col_key_cut_decision' <<<"$out"
finish
