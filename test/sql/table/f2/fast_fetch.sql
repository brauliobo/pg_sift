-- Rows of a key-ordered Format 2.0 sift_table read through a secondary index: the address of a row is found through the hash directory of its part (SEC_TID_HASH) and not by reading the granules whose
-- addresses cover it, every row is the one of the heap twin whether the plan is the index, the bitmap or the scan of the table, and the planner prices the fetch of a row of a granule: a lookup of one row
-- stays on the index, ranges of hundreds of rows and more go to the scan.
-- requires: build col
CREATE EXTENSION pg_sift_test CASCADE;
DO $$ BEGIN PERFORM set_config('sift.test_create_format1', 'off', false); EXCEPTION WHEN OTHERS THEN NULL; END $$;
CREATE FUNCTION tid_hash_stats(bool) RETURNS text LANGUAGE C AS 'pg_sift', 'sift_test_f2_tid_hash_stats';

CREATE TABLE ff (id int PRIMARY KEY, k int, grp int, name text, amount float8) USING sift_table WITH (autovacuum_enabled = false);
SELECT sift.table_set_option('ff', 'key_order', 'k');
INSERT INTO ff SELECT g, (g * 7919) % 60013, g % 50, 'name-' || (g * 31 % 99991), sin(g) * 1000 FROM generate_series(1, 60000) g;
CREATE INDEX ff_k ON ff (k);
CREATE TABLE ff_h (LIKE ff);
INSERT INTO ff_h SELECT * FROM ff;
ALTER TABLE ff_h ADD PRIMARY KEY (id);
CREATE INDEX ff_h_k ON ff_h (k);
SELECT sift.table_maintain('ff') IS NOT NULL AS maintained;
ANALYZE ff;
ANALYZE ff_h;

CREATE FUNCTION plan_node(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (COSTS off) ' || q LOOP
		RETURN regexp_replace(line, '^\s+|\s+(using|on) .*$', '', 'g');
	END LOOP;
END $$;

-- the rows of the table and of its twin, by the columns of the key: the same text for any plan
CREATE FUNCTION rows_md5(tbl text, pred text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT md5(string_agg(t::text, ''|'' ORDER BY id)) FROM %I t WHERE %s', tbl, pred) INTO r;
	RETURN r;
END $$;

SELECT tid_hash_stats(true) AS reset;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*) AS point_lookups,
	bool_and(rows_md5('ff', 'id = ' || g) = rows_md5('ff_h', 'id = ' || g)) AS same_rows_as_heap
FROM generate_series(1, 60000, 311) g;
SELECT split_part(s, ' ', 1)::int > 0 AS used_the_hash_directory, split_part(s, ' ', 2)::float8 / split_part(s, ' ', 1)::int <= 4 AS at_most_a_bucket_of_granules FROM tid_hash_stats(false) s;
RESET enable_seqscan;
RESET enable_bitmapscan;

SELECT n, rows_md5('ff', 'id BETWEEN 777 AND ' || (776 + n)) = rows_md5('ff_h', 'id BETWEEN 777 AND ' || (776 + n)) AS id_range_same,
	rows_md5('ff', 'k BETWEEN 1000 AND ' || (999 + n)) = rows_md5('ff_h', 'k BETWEEN 1000 AND ' || (999 + n)) AS k_range_same,
	plan_node('SELECT * FROM ff WHERE id BETWEEN 777 AND ' || (776 + n)) AS id_plan, plan_node('SELECT * FROM ff WHERE k BETWEEN 1000 AND ' || (999 + n)) AS k_plan
FROM unnest(ARRAY[1, 500, 2000, 5000, 10000]) n;

SET enable_seqscan = off;
SELECT n, rows_md5('ff', 'k BETWEEN 1000 AND ' || (999 + n)) = rows_md5('ff_h', 'k BETWEEN 1000 AND ' || (999 + n)) AS forced_index_same FROM unnest(ARRAY[10, 500, 5000]) n;
RESET enable_seqscan;

DROP FUNCTION rows_md5(text, text), plan_node(text), tid_hash_stats(bool);
DROP TABLE ff, ff_h;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
