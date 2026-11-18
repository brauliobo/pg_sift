-- A key_cut build of a table of several runs reads the heap in the order of a btree on the key when that is cheaper than the key ranges (sift_col build btree.c,
-- btree_pick.c): same rows, a key in the units the ranges give, no temporary file. The choice plays the heap blocks of the btree's entries through a cache and
-- says what it would read; a btree that does not give the key's order (descending, another first column, a predicate, an expression, another type) is never used.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.async_maintenance = off;
SET sift.test_col_segment_kb = 1500;
SET sift.col_unit_rows = 1200;
SET sift.test_col_pick_cache_blocks = 64;

-- bt_sorted: k follows the heap; bt_noisy: half of it is displaced; bt_random: none of it follows
SELECT setseed(0.42);
CREATE TABLE bt_sorted (id int, k numeric(10,2), g int, note text, pad text) WITH (autovacuum_enabled = false);
CREATE TABLE bt_noisy (LIKE bt_sorted) WITH (autovacuum_enabled = false);
CREATE TABLE bt_random (LIKE bt_sorted) WITH (autovacuum_enabled = false);
INSERT INTO bt_sorted SELECT i, i, i % 7, 'note ' || (i % 50), md5(i::text) || md5((i + 1)::text) FROM generate_series(1, 36000) i;
INSERT INTO bt_noisy SELECT i, round((0.5 * i + 0.5 * 36000 * random())::numeric, 2), i % 7, 'note ' || (i % 50), md5(i::text) || md5((i + 1)::text) FROM generate_series(1, 36000) i;
INSERT INTO bt_random SELECT i, round((36000 * random())::numeric, 2), i % 7, 'note ' || (i % 50), md5(i::text) || md5((i + 1)::text) FROM generate_series(1, 36000) i;
CREATE INDEX bt_sorted_k ON bt_sorted (k);
CREATE INDEX bt_noisy_k ON bt_noisy (k);
CREATE INDEX bt_random_k ON bt_random (k);
VACUUM (FREEZE, ANALYZE) bt_sorted, bt_noisy, bt_random;

CREATE FUNCTION bt_build(t regclass, mode int) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('DROP INDEX IF EXISTS %s_idx', t);
	PERFORM set_config('sift.test_col_build_btree', mode::text, true);
	EXECUTE format('CREATE INDEX %s_idx ON %s USING sift_col (id, k, g, note, pad) WITH (key_cut = ''k'')', t, t);
	RETURN CASE WHEN current_setting('sift.test_col_btree_entries')::int > 0 THEN 'btree' ELSE 'ranges' END;
END $$;

CREATE FUNCTION bt_same(t regclass) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a text; b text; q text;
BEGIN
	FOREACH q IN ARRAY ARRAY[format('SELECT count(*), sum(id), min(note) FROM %s WHERE k BETWEEN 100 AND 5000', t), format('SELECT g, count(*), sum(k) FROM %s WHERE k > 30000 GROUP BY 1 ORDER BY 1', t)] LOOP
		SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexscan = off; SET LOCAL sift.enable_agg = on;
		EXECUTE format('SELECT string_agg(x::text, E''\n'') FROM (%s) x', q) INTO a;
		SET LOCAL sift.enable_agg = off; SET LOCAL enable_seqscan = on;
		EXECUTE format('SELECT string_agg(x::text, E''\n'') FROM (%s) x', q) INTO b;
		IF a IS DISTINCT FROM b THEN RETURN false; END IF;
	END LOOP;
	RETURN true;
END $$;

CREATE FUNCTION bt_units(t regclass) RETURNS numeric LANGUAGE sql AS $$
	SELECT round(point_units::numeric, 1) FROM sift.col_clustering(format('%s_idx', t)::regclass, 1) WHERE path = 'k' $$;

-- the choice: a table in the order of the key reads each block once; a displaced one reads blocks again and the key ranges are cheaper
SELECT 'bt_sorted' AS tbl, bt_build('bt_sorted', 0) AS path, current_setting('sift.test_col_pick_reads')::int > 0 AS predicted;
SELECT 'bt_noisy' AS tbl, bt_build('bt_noisy', 0) AS path;
SELECT 'bt_random' AS tbl, bt_build('bt_random', 0) AS path;

-- the prediction against the heap: with a cache that holds the heap, the blocks read are the blocks of the table (each once), and the walk counts the changes of the
-- block along the entries as the build's stream asks for them; with no cache every change is a read
SET sift.test_col_pick_cache_blocks = 60000;
SELECT bt_build('bt_noisy', 1) AS path;
SELECT current_setting('sift.test_col_pick_reads')::int = pg_relation_size('bt_noisy') / 8192 AS predicted_is_the_heap,
       current_setting('sift.test_col_pick_blocks') = current_setting('sift.test_col_btree_blocks') AS stream_is_the_walk,
       current_setting('sift.test_col_pick_entries')::int = 36000 AS every_entry;
SET sift.test_col_pick_cache_blocks = 0;
SELECT bt_build('bt_noisy', 1) AS path;
SELECT current_setting('sift.test_col_pick_reads') = current_setting('sift.test_col_pick_blocks') AS no_cache_reads_every_change;
SET sift.test_col_pick_cache_blocks = 64;

-- the same rows and units as the key ranges give, and no temporary file
SELECT pg_stat_force_next_flush();
SELECT temp_files AS temp_files_before FROM pg_stat_database WHERE datname = current_database() \gset
SELECT bt_build('bt_sorted', 1) AS path, current_setting('sift.test_col_spill_bytes') AS spill_bytes;
SELECT bt_units('bt_sorted') AS units_btree, sift.col_maintenance_due('bt_sorted_idx') AS due, sift.col_verify('bt_sorted_idx', true);
SELECT bt_same('bt_sorted') AS same_rows;
SELECT bt_build('bt_noisy', 1) AS path, current_setting('sift.test_col_spill_bytes') AS spill_bytes;
SELECT bt_units('bt_noisy') AS units_btree, sift.col_verify('bt_noisy_idx', true);
SELECT bt_same('bt_noisy') AS same_rows;
SELECT pg_stat_force_next_flush();
SELECT temp_files = :temp_files_before AS no_temp_files FROM pg_stat_database WHERE datname = current_database();
SELECT bt_build('bt_noisy', -1) AS path, current_setting('sift.test_col_spill_bytes')::int > 0 AS spilled;
SELECT bt_units('bt_noisy') AS units_ranges;

-- a btree that does not give the key's order is not used, whatever the cost: descending, another first column, a predicate, an expression, NULLs first, a key of another type
CREATE TABLE bt_gate (id int, k numeric(10,2), g int, note text, pad text, t text) WITH (autovacuum_enabled = false);
INSERT INTO bt_gate SELECT i, round((0.5 * i + 0.5 * 36000 * random())::numeric, 2), i % 7, 'note ' || (i % 50), md5(i::text) || md5((i + 1)::text), (i % 977)::text FROM generate_series(1, 36000) i;
VACUUM (FREEZE, ANALYZE) bt_gate;
CREATE INDEX bt_gate_desc ON bt_gate (k DESC);
CREATE INDEX bt_gate_second ON bt_gate (g, k);
CREATE INDEX bt_gate_partial ON bt_gate (k) WHERE g < 5;
CREATE INDEX bt_gate_expr ON bt_gate ((k + 1));
CREATE INDEX bt_gate_nulls_first ON bt_gate (k NULLS FIRST);
CREATE INDEX bt_gate_text ON bt_gate (t);
SELECT bt_build('bt_gate', 1) AS only_wrong_order_btrees;
-- the mutant that opens the gate takes one of them, which the test would not let pass: the gate is what keeps them out
SET sift.test_col_btree_gate_open = on;
SELECT bt_build('bt_gate', 1) AS mutant_path;
SET sift.test_col_btree_gate_open = off;
-- a btree on the key with other columns after it serves (the rows of one value are sorted in the memory of the run), an invalid one does not
CREATE INDEX bt_gate_prefix ON bt_gate (k, g DESC, (id + 1));
SELECT bt_build('bt_gate', 1) AS prefix_match;
SELECT bt_same('bt_gate') AS same_rows, sift.col_verify('bt_gate_idx', true);
UPDATE pg_index SET indisvalid = false WHERE indexrelid = 'bt_gate_prefix'::regclass;
SELECT bt_build('bt_gate', 1) AS invalid_btree;
UPDATE pg_index SET indisvalid = true WHERE indexrelid = 'bt_gate_prefix'::regclass;

RESET sift.test_col_segment_kb;
DROP TABLE bt_sorted, bt_noisy, bt_random, bt_gate;
DROP FUNCTION bt_build(regclass, int), bt_same(regclass), bt_units(regclass);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
