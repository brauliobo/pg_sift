#!/usr/bin/env bash
# The sample of the automatic key_cut on the 8-page-superblock flavour of scripts/scaling-gate.sh (units of 8 pages, 2,328 rows at most), where a heap of 116 superblocks has 928 rows:
#  - the first look of a table of 116 superblocks sampled 7 of the 8 it plans and ended at "fewer than 16 superblocks" when a planned superblock held no row (the empty pages a
#    bulk load leaves at the end, a deleted and vacuumed range): the same count of superblocks and the same stride of the stretches, at 928 rows and not 4,000,000. A superblock with
#    no row gives its place to the next that has rows, and the stretches past the end of the heap take the last ones that have rows;
#  - the memory of the decision is bounded by maintenance_work_mem and a constant, whatever the rows sampled: at maintenance_work_mem = 1MB and five sizes of sample (64 to 126
#    superblocks of 2,328 rows, 150,000 to 293,000 rows, five to ten times the memory they may keep), the exact bytes of each stage (the scopes col_key_cut_decision = the sample,
#    col_key_cut_cells, col_key_cut_layout; sift._test_mem_highwater) are the same bytes at every size (a fitted term of 0 bytes a row; the layout adds 16,432 bytes for each of the
#    superblocks it merges), under the bound of the code, and the rows past the memory are on temporary files;
#  - the bound follows maintenance_work_mem (1, 2 and 4 MB), and a mutant whose spools keep every chunk in memory (sift.test_key_cut_unbounded) breaks it;
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

# kx(sbs, pad): a table of sbs superblocks of 8 pages (the rows a page holds measured on 4,000 rows of its width, 291 for pad 0: 2,328 rows a superblock, the most a unit holds); kxi(leaves): its index of 2 to 4 leaves
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
-- the decision of kx_i with the memory it may keep (mwm) and the exact bytes each stage held at the most: the sample, the cells of a leaf, the layouts
CREATE FUNCTION kd(leaves int, mwm text, sbs int) RETURNS void LANGUAGE plpgsql AS \$f\$
DECLARE s int; sp int8; cp int8; lp int8;
BEGIN
	PERFORM set_config('maintenance_work_mem', mwm, true);
	PERFORM * FROM sift._test_mem_highwater(true);
	SELECT d.sampled INTO s FROM sift.key_cut_decision('kx_i') d;
	SELECT h.peak INTO sp FROM sift._test_mem_highwater(false) h WHERE h.scope = 'col_key_cut_decision';
	SELECT h.peak INTO cp FROM sift._test_mem_highwater(false) h WHERE h.scope = 'col_key_cut_cells';
	SELECT h.peak INTO lp FROM sift._test_mem_highwater(false) h WHERE h.scope = 'col_key_cut_layout';
	INSERT INTO kres VALUES (sbs, leaves, current_setting('maintenance_work_mem'), s, s::int8 * 2328, (s + (s + 47) / 48 - 1) / ((s + 47) / 48), sp, cp, lp);
END \$f\$;
-- the verdict of the decision, every number, at two sizes of the memory it may keep
CREATE FUNCTION kdiff() RETURNS bool LANGUAGE plpgsql AS \$f\$
DECLARE a text; b text;
BEGIN
	PERFORM set_config('maintenance_work_mem', '1MB', true);
	SELECT d::text INTO a FROM sift.key_cut_decision('kx_i') d;
	PERFORM set_config('maintenance_work_mem', '1GB', true);
	SELECT d::text INTO b FROM sift.key_cut_decision('kx_i') d;
	RETURN a = b AND a LIKE '%cut%';
END \$f\$;
CREATE TABLE kres (sbs int, leaves int, mwm text, sampled int, sample_rows int8, nused int, s_peak int8, c_peak int8, l_peak int8);" >/dev/null || bail "functions failed"

plan 18
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

# the exact memory of the decision at maintenance_work_mem = 1MB
for sbs in 256 320 384 448 512; do
	sql ks "SELECT kx($sbs, 0)" "VACUUM (FREEZE, ANALYZE) kx" "SELECT kxi(3)" "SELECT kd(3, '1MB', $sbs)" >/dev/null || bail "fixture $sbs failed"
done
# the code's bound for the stage (R = 2,328 rows of a superblock, recsz = 8 * (2 + leaves) bytes of a sampled row, mwm = 1MB):
#   sample  mwm + R * recsz + 192 kB          (the superblock being read, the tracks, the state of the index)
#   cells   mwm + 8 * R + 16,432 * sampled + 64 kB   (the sorted values of a superblock, a buffer for each run on the file)
#   layout  mwm + R * (recsz + 64) + 4 * R * (recsz + 24) + 4 * R * 88 + 16,432 * nused + 64 kB (the superblock being sorted, the window of the merge, the columns of a unit, a buffer for each run)
read -r sb_max cb_max lb_max s_spread c_spread l_spread s_slope c_slope l_slope s_flat c_flat l_flat <<<"$(sql ks "
SELECT max(s_peak - (1048576 + 2328 * 40 + 196608)), max(c_peak - (1048576 + 8 * 2328 + 16432 * sampled + 65536)),
	max(l_peak - (1048576 + 2328 * (40 + 64) + 4 * 2328 * (40 + 24) + 4 * 2328 * 88 + 16432 * nused + 65536)),
	max(s_peak) - min(s_peak), max(c_peak - 16432 * sampled) - min(c_peak - 16432 * sampled), max(l_peak - 16432 * nused) - min(l_peak - 16432 * nused),
	round(regr_slope(s_peak, sample_rows)::numeric, 3), round(regr_slope(c_peak - 16432 * sampled, sample_rows)::numeric, 3), round(regr_slope(l_peak - 16432 * nused, sample_rows)::numeric, 3),
	abs(regr_slope(s_peak, sample_rows)) < 0.1, abs(regr_slope(c_peak - 16432 * sampled, sample_rows)) < 0.1, abs(regr_slope(l_peak - 16432 * nused, sample_rows)) < 0.1
FROM kres WHERE mwm = '1MB'" | tr '|' ' ')"
check "the sample stage held no more than its bound at five sizes of sample (the largest 293,000 rows of 40 bytes, 11.7 MB)" test "$sb_max" -le 0
check "the cells of the leaf held no more than their bound" test "$cb_max" -le 0
check "the layouts held no more than their bound" test "$lb_max" -le 0
check "the sample stage held the same bytes at every size of sample (spread $s_spread bytes, fitted $s_slope bytes a row)" test "$s_spread" -le 8192 -a "$s_flat" == t
check "the cells held the same bytes at every size of sample past the 16,432 bytes of each run merged (spread $c_spread bytes, fitted $c_slope bytes a row)" test "$c_spread" -le 8192 -a "$c_flat" == t
check "the layouts held the same bytes at every size of sample past the 16,432 bytes of each superblock merged (spread $l_spread bytes, fitted $l_slope bytes a row)" test "$l_spread" -le 8192 -a "$l_flat" == t
out=$(sql ks "SELECT min(sampled), max(sampled), min(s_peak) > 1048576 FROM kres WHERE mwm = '1MB'" | tr '|' ' ')
check "the samples were 64 to 126 superblocks and the sample stage held the memory it may keep" test "$out" == "64 126 t"
out=$(sql ks "SELECT kdiff()")
check "the verdict, its reads, ideal units and predicted growth to the last digit, is the same at 1 MB and at 1 GB of maintenance_work_mem" test "$out" == t
# the rows past the memory are on temporary files
sql ks "SELECT pg_stat_force_next_flush()" >/dev/null
t0=$(sql ks "SELECT temp_files FROM pg_stat_database WHERE datname = current_database()")
sql ks "SELECT kd(3, '1MB', 0)" >/dev/null && sql ks "SELECT pg_stat_force_next_flush()" >/dev/null
t1=$(sql ks "SELECT temp_files FROM pg_stat_database WHERE datname = current_database()")
check "the decision wrote temporary files for the rows past maintenance_work_mem" test $((t1 - t0)) -ge 3
# the bound follows maintenance_work_mem
for mwm in 2MB 4MB; do sql ks "SELECT kd(3, '$mwm', 384)" >/dev/null || bail "mwm $mwm failed"; done
out=$(sql ks "SELECT mwm, s_peak - CASE mwm WHEN '1MB' THEN 1048576 WHEN '2MB' THEN 2097152 ELSE 4194304 END <= 2328 * 40 + 196608 FROM kres WHERE sbs IN (384) ORDER BY 1" | tr '\n|' '  ')
check "the sample stage held its bound at 1, 2 and 4 MB of maintenance_work_mem" test "$out" == "1MB t 2MB t 4MB t "
out=$(sql ks "SELECT s_peak FROM kres WHERE sbs = 384 AND mwm = '4MB'")
check "and at 4 MB it held more than at 1 MB: the bound is maintenance_work_mem" test "$out" -ge 4194304
# the mutant: the spools keep every chunk in memory, as a decision that ignored the setting would
sql ks "SET sift.test_key_cut_unbounded = on" "SELECT kd(3, '1MB', 1)" >/dev/null || bail "mutant failed"
out=$(sql ks "SELECT s_peak > 1048576 + 2328 * 40 + 196608 FROM kres WHERE sbs = 1")
check "the mutant breaks the bound of the sample stage" test "$out" == t
sql ks "SELECT kx(64, 150)" "VACUUM (FREEZE, ANALYZE) kx" "SELECT kxi(3)" >/dev/null
out=$(sql ks "SET sift.test_mem_budget_scale = 0.0001" "SELECT * FROM sift.key_cut_decision('kx_i')" 2>&1)
check "a budget of a ten-thousandth fails the decision by name" grep -q 'sift memory budget exceeded: col_key_cut' <<<"$out"
finish
