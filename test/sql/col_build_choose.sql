-- Where the rows of a CREATE INDEX or REINDEX of a sift_col index come from (col/build/choose.c): the heap (with the key ranges' files for a key_cut index), the heap read in
-- the order of a btree on the key, or the index a REINDEX replaces. Each path is priced and the cheapest taken; sift.col_build_source = heap takes the heap. Every path gives the
-- same rows and the key in one unit; the path that reads a btree or the old index writes no temporary file, and the old index reads no block of the heap.
-- sift.test_col_build_source_used says which path the last build took (0 the heap, 1 a btree, 2 the old index); the mutants plant a defect in the choice that a step must catch.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.async_maintenance = off;
SET sift.test_col_segment_kb = 1500;
SET sift.col_unit_rows = 1200;
SET sift.test_col_pick_cache_blocks = 64;

-- ch_sorted: k follows the heap; ch_noisy: half of it is displaced; ch_plain: k follows the heap and there is no btree
SELECT setseed(0.42);
CREATE TABLE ch_sorted (id int, k numeric(10,2), g int, note text, pad text) WITH (autovacuum_enabled = false);
CREATE TABLE ch_noisy (LIKE ch_sorted) WITH (autovacuum_enabled = false);
CREATE TABLE ch_plain (LIKE ch_sorted) WITH (autovacuum_enabled = false);
INSERT INTO ch_sorted SELECT i, i, i % 7, 'note ' || (i % 50), md5(i::text) || md5((i + 1)::text) FROM generate_series(1, 36000) i;
INSERT INTO ch_noisy SELECT i, round((0.5 * i + 0.5 * 36000 * random())::numeric, 2), i % 7, 'note ' || (i % 50), md5(i::text) || md5((i + 1)::text) FROM generate_series(1, 36000) i;
INSERT INTO ch_plain SELECT * FROM ch_sorted;
CREATE INDEX ch_sorted_k ON ch_sorted (k);
CREATE INDEX ch_noisy_k ON ch_noisy (k);
VACUUM (FREEZE, ANALYZE) ch_sorted, ch_noisy, ch_plain;

CREATE FUNCTION ch_path() RETURNS text LANGUAGE sql AS
$$ SELECT (ARRAY['heap', 'btree', 'old index'])[1 + current_setting('sift.test_col_build_source_used')::int] $$;

-- a build of the table's index: the path it took, whether it wrote temporary files and whether it accessed a block of the heap
CREATE TABLE ch_mark (heap_buffers bigint, temp_bytes bigint);
CREATE FUNCTION ch_start(t regclass) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	TRUNCATE ch_mark;
	INSERT INTO ch_mark SELECT coalesce((SELECT sum(heap_blks_read + heap_blks_hit) FROM pg_statio_user_tables WHERE relid = t), 0),
		(SELECT temp_bytes FROM pg_stat_database WHERE datname = current_database());
END $$;
CREATE FUNCTION ch_note(t regclass) RETURNS TABLE (path text, temp_files bool, reads_heap bool) LANGUAGE sql AS $$
	SELECT ch_path(), (SELECT temp_bytes FROM pg_stat_database WHERE datname = current_database()) > m.temp_bytes,
		coalesce((SELECT sum(heap_blks_read + heap_blks_hit) FROM pg_statio_user_tables WHERE relid = t), 0) - m.heap_buffers > 0 FROM ch_mark m $$;

\set ci_sorted 'CREATE INDEX ch_sorted_idx ON ch_sorted USING sift_col (id, k, g, note, pad) WITH (key_cut = ''k'')'
\set ci_noisy 'CREATE INDEX ch_noisy_idx ON ch_noisy USING sift_col (id, k, g, note, pad) WITH (key_cut = ''k'')'
\set ci_plain 'CREATE INDEX ch_plain_idx ON ch_plain USING sift_col (id, k, g, note, pad) WITH (key_cut = ''k'')'

CREATE FUNCTION ch_same(t regclass) RETURNS bool LANGUAGE plpgsql AS $$
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

CREATE FUNCTION ch_units(t regclass) RETURNS numeric LANGUAGE sql AS $$
	SELECT round(point_units::numeric, 1) FROM sift.col_clustering(format('%s_idx', t)::regclass, 1) WHERE path = 'k' $$;

-- the table in the order of the key, with a btree on it: CREATE INDEX reads the heap through the btree, the rows of the old index are cheaper still for the REINDEX that follows
SELECT pg_stat_force_next_flush() \gset
SELECT pg_stat_force_next_flush() \gset
SELECT ch_start('ch_sorted');
:ci_sorted;
SELECT pg_stat_force_next_flush() \gset
SELECT * FROM ch_note('ch_sorted');
SELECT ch_units('ch_sorted') AS units, ch_same('ch_sorted') AS same_rows, sift.col_verify('ch_sorted_idx', true);
SELECT pg_stat_force_next_flush() \gset
SELECT pg_stat_force_next_flush() \gset
SELECT ch_start('ch_sorted');
REINDEX INDEX ch_sorted_idx;
SELECT pg_stat_force_next_flush() \gset
SELECT * FROM ch_note('ch_sorted');
SELECT ch_units('ch_sorted') AS units, ch_same('ch_sorted') AS same_rows, sift.col_verify('ch_sorted_idx', true);

-- the opt-out reads the heap for both, with the key ranges' files
SET sift.col_build_source = heap;
DROP INDEX ch_sorted_idx;
SELECT pg_stat_force_next_flush() \gset
SELECT pg_stat_force_next_flush() \gset
SELECT ch_start('ch_sorted');
:ci_sorted;
SELECT pg_stat_force_next_flush() \gset
SELECT * FROM ch_note('ch_sorted');
SELECT ch_units('ch_sorted') AS units, ch_same('ch_sorted') AS same_rows, sift.col_verify('ch_sorted_idx', true);
SELECT pg_stat_force_next_flush() \gset
SELECT pg_stat_force_next_flush() \gset
SELECT ch_start('ch_sorted');
REINDEX INDEX ch_sorted_idx;
SELECT pg_stat_force_next_flush() \gset
SELECT * FROM ch_note('ch_sorted');
RESET sift.col_build_source;

-- a table whose key is displaced: the key ranges are cheaper than the btree for CREATE INDEX, and the old index is cheaper than the key ranges for the REINDEX
SELECT pg_stat_force_next_flush() \gset
SELECT pg_stat_force_next_flush() \gset
SELECT ch_start('ch_noisy');
:ci_noisy;
SELECT pg_stat_force_next_flush() \gset
SELECT * FROM ch_note('ch_noisy');
SELECT pg_stat_force_next_flush() \gset
SELECT pg_stat_force_next_flush() \gset
SELECT ch_start('ch_noisy');
REINDEX INDEX ch_noisy_idx;
SELECT pg_stat_force_next_flush() \gset
SELECT * FROM ch_note('ch_noisy');
SELECT ch_units('ch_noisy') AS units, ch_same('ch_noisy') AS same_rows, sift.col_verify('ch_noisy_idx', true);

-- no btree at all: the heap for CREATE INDEX, the old index for REINDEX
SELECT pg_stat_force_next_flush() \gset
SELECT pg_stat_force_next_flush() \gset
SELECT ch_start('ch_plain');
:ci_plain;
SELECT pg_stat_force_next_flush() \gset
SELECT * FROM ch_note('ch_plain');
SELECT pg_stat_force_next_flush() \gset
SELECT pg_stat_force_next_flush() \gset
SELECT ch_start('ch_plain');
REINDEX INDEX ch_plain_idx;
SELECT pg_stat_force_next_flush() \gset
SELECT * FROM ch_note('ch_plain');
SELECT ch_units('ch_plain') AS units, ch_same('ch_plain') AS same_rows, sift.col_verify('ch_plain_idx', true);

-- an old index that cannot give the rows (another column format minor is asked for) is built from the heap
SET sift.col_format_minor = 14;
SELECT pg_stat_force_next_flush() \gset
SELECT pg_stat_force_next_flush() \gset
SELECT ch_start('ch_sorted');
REINDEX INDEX ch_sorted_idx;
SELECT pg_stat_force_next_flush() \gset
SELECT * FROM ch_note('ch_sorted');
SELECT pg_stat_force_next_flush() \gset
SELECT pg_stat_force_next_flush() \gset
SELECT ch_start('ch_plain');
REINDEX INDEX ch_plain_idx;
SELECT pg_stat_force_next_flush() \gset
SELECT * FROM ch_note('ch_plain');
RESET sift.col_format_minor;
SELECT ch_same('ch_sorted') AS same_rows, sift.col_verify('ch_sorted_idx', true);

-- rows this transaction has buffered are not in the index that is replaced
BEGIN;
INSERT INTO ch_plain SELECT i, i, i % 7, 'note ' || (i % 50), md5(i::text) FROM generate_series(36001, 36100) i;
REINDEX INDEX ch_plain_idx;
SELECT ch_path() AS path_with_own_rows;
COMMIT;
SELECT ch_same('ch_plain') AS same_rows, sift.col_verify('ch_plain_idx', true);

-- the messages: what was chosen, and what each path would have taken (the costs are set, not measured: a read 80 microseconds at random, 10 in order)
SET client_min_messages = notice;
\set VERBOSITY default
DROP INDEX ch_sorted_idx;
CREATE INDEX ch_sorted_idx ON ch_sorted USING sift_col (id, k, g, note, pad) WITH (key_cut = 'k');
REINDEX INDEX ch_sorted_idx;
DROP INDEX ch_noisy_idx;
CREATE INDEX ch_noisy_idx ON ch_noisy USING sift_col (id, k, g, note, pad) WITH (key_cut = 'k');
SET sift.test_col_temp_free_kb = 100;
DROP INDEX ch_noisy_idx;
CREATE INDEX ch_noisy_idx ON ch_noisy USING sift_col (id, k, g, note, pad) WITH (key_cut = 'k');
RESET sift.test_col_temp_free_kb;
\set VERBOSITY terse
SET client_min_messages = warning;

-- the mutants: the dearest path is taken (the noisy table goes through its btree, the REINDEX does not take the old index), the room of the files is not looked at (the build is refused)
SET sift.test_col_choose_mutant = 1;
DROP INDEX ch_noisy_idx;
SELECT pg_stat_force_next_flush() \gset
SELECT pg_stat_force_next_flush() \gset
SELECT ch_start('ch_noisy');
:ci_noisy;
SELECT pg_stat_force_next_flush() \gset
SELECT * FROM ch_note('ch_noisy');
SELECT pg_stat_force_next_flush() \gset
SELECT pg_stat_force_next_flush() \gset
SELECT ch_start('ch_noisy');
REINDEX INDEX ch_noisy_idx;
SELECT pg_stat_force_next_flush() \gset
SELECT * FROM ch_note('ch_noisy');
SET sift.test_col_choose_mutant = 0;
SET sift.test_col_temp_free_kb = 100;
DROP INDEX ch_noisy_idx;
SELECT pg_stat_force_next_flush() \gset
SELECT pg_stat_force_next_flush() \gset
SELECT ch_start('ch_noisy');
:ci_noisy;
SELECT pg_stat_force_next_flush() \gset
SELECT * FROM ch_note('ch_noisy');
SET sift.test_col_choose_mutant = 2;
DROP INDEX ch_noisy_idx;
CREATE FUNCTION ch_refused() RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE 'CREATE INDEX ch_noisy_idx ON ch_noisy USING sift_col (id, k, g, note, pad) WITH (key_cut = ''k'')';
	RETURN 'built';
EXCEPTION WHEN program_limit_exceeded THEN RETURN 'refused';
END $$;
SELECT ch_refused() AS room_ignored;
SET sift.test_col_choose_mutant = 0;
RESET sift.test_col_temp_free_kb;
DROP FUNCTION ch_refused();

-- a key that is not a number cannot be cut by (its order under a collation is not the order the runs are cut in), so no btree on a text column is a candidate: the index is refused
-- at CREATE INDEX, and a btree on a text column of a table whose key is a number is not read (test col_key_cut_btree: the gate)
CREATE TABLE ch_text (id int, t text COLLATE "C", u text, pad text) WITH (autovacuum_enabled = false);
INSERT INTO ch_text SELECT i, lpad(i::text, 8, '0'), md5(i::text), md5(i::text) || md5((i + 1)::text) FROM generate_series(1, 3600) i;
CREATE INDEX ch_text_t ON ch_text (t);
CREATE INDEX ch_text_u ON ch_text (u COLLATE "en-x-icu");
SELECT 1 FROM ch_text LIMIT 0;
\set VERBOSITY terse
CREATE INDEX ch_text_idx ON ch_text USING sift_col (id, t, u, pad) WITH (key_cut = 't');
DROP TABLE ch_text;

-- dead tuples: every entry of the btree costs a visit of the heap, dead or not, and the rows are those of the plain scan
CREATE TABLE ch_bloat (LIKE ch_sorted) WITH (autovacuum_enabled = false);
INSERT INTO ch_bloat SELECT * FROM ch_sorted;
CREATE INDEX ch_bloat_k ON ch_bloat (k);
UPDATE ch_bloat SET note = 'updated ' || note WHERE id % 2 = 0;
DELETE FROM ch_bloat WHERE id % 5 = 0;
SET sift.test_col_build_btree = 1;
SELECT pg_stat_force_next_flush() \gset
SELECT pg_stat_force_next_flush() \gset
SELECT ch_start('ch_bloat');
CREATE INDEX ch_bloat_idx ON ch_bloat USING sift_col (id, k, g, note, pad) WITH (key_cut = 'k');
SELECT pg_stat_force_next_flush() \gset
SELECT * FROM ch_note('ch_bloat');
SELECT current_setting('sift.test_col_btree_entries')::int > current_setting('sift.test_col_btree_visible')::int AS entries_of_dead_tuples,
       current_setting('sift.test_col_btree_visible')::int = (SELECT count(*) FROM ch_bloat) AS visible_are_the_rows;
SELECT ch_units('ch_bloat') AS units, ch_same('ch_bloat') AS same_rows, sift.col_verify('ch_bloat_idx', true);
RESET sift.test_col_build_btree;
DROP TABLE ch_bloat;

-- a scoped index: the btree serves the rows of its entities, an index with a scope has no old index that can give them
CREATE TABLE ch_scope (member int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (member, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO ch_scope SELECT m, 1 + (m * 31 + j * 13) % 40 FROM generate_series(1, 1500) m, generate_series(0, 5) j WHERE j < m % 7;
CREATE INDEX ch_scope_journal ON ch_scope USING sift_col (member, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE ch_facts (id int PRIMARY KEY, member int, k numeric(10,2), pad text) WITH (autovacuum_enabled = false);
INSERT INTO ch_facts SELECT i, 1 + i % 1500, i, md5(i::text) || md5((i + 1)::text) FROM generate_series(1, 36000) i;
CREATE INDEX ch_facts_k ON ch_facts (k);
VACUUM (FREEZE, ANALYZE) ch_facts;
CREATE INDEX ch_facts_idx ON ch_facts USING sift_col (id, member, k, pad) WITH (key_cut = 'k', scope = 'tenant_id: member -> ch_scope(member, tenant_id)');
SELECT ch_path() AS path_scoped_create;
REINDEX INDEX ch_facts_idx;
SELECT ch_path() AS path_scoped_reindex;
SELECT sift.col_verify('ch_facts_idx', true);
DROP TABLE ch_facts, ch_scope;

RESET sift.test_col_segment_kb;
DROP TABLE ch_sorted, ch_noisy, ch_plain, ch_mark;
DROP FUNCTION ch_path(), ch_start(regclass), ch_note(regclass), ch_same(regclass), ch_units(regclass);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
