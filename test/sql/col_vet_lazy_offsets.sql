-- A unit with a page that is not all-visible checks the rows it selects against the heap, a page at a time. It decoded the row offsets of every row of the unit first
-- (60 to 90 instructions a row, 2 million for the two units of 22,000 rows that a month of one entity reads), though the 44 rows it checks sit on 44 pages: the offsets
-- of the pages that hold a selected row are decoded, and no other. A table that was never vacuumed (or a standby, which cannot VACUUM) is in that state at once.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET default_statistics_target = 10000;
SELECT setseed(0.42);

CREATE TABLE vl (id int PRIMARY KEY, dim_a_id int NOT NULL, foreign_id_b int NOT NULL, d date NOT NULL, score int NOT NULL, body text) WITH (autovacuum_enabled = false);
INSERT INTO vl SELECT g, 1 + (power(random(), 2) * 99)::int, (random() * 50)::int, DATE '2022-01-01' + (random() * 1000)::int, (random() * 5)::int, md5(g::text) FROM generate_series(1, 200000) g;
CREATE INDEX vl_col ON vl USING sift_col (dim_a_id, foreign_id_b, d, score) WITH (key_cut = 'dim_a_id', rollup_dims = 'off');
ANALYZE vl;
SELECT relallvisible FROM pg_class WHERE relname = 'vl';

CREATE FUNCTION vl_scan(q text) RETURNS TABLE (rows_equal boolean, offsets_decoded bigint, rows_scanned bigint) LANGUAGE plpgsql AS $$
DECLARE a text; b text; ex jsonb; n bigint;
BEGIN
	SET LOCAL enable_seqscan = off;
	SET LOCAL enable_indexscan = off;
	SET LOCAL enable_bitmapscan = off;
	EXECUTE 'SELECT md5(string_agg(t::text, '','' ORDER BY t::text)) FROM (' || q || ') t' INTO a;
	PERFORM sift._test_vet_offsets(true);
	EXECUTE 'EXPLAIN (ANALYZE, FORMAT JSON, COSTS OFF, TIMING OFF) ' || q INTO ex;
	n := sift._test_vet_offsets(true);
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT md5(string_agg(t::text, '','' ORDER BY t::text)) FROM (' || q || ') t' INTO b;
	RETURN QUERY SELECT a = b, n, (regexp_match(ex::text, '"Rows Scanned": (\d+)'))[1]::bigint;
END $$;

-- one month of one entity: tens of rows selected out of the tens of thousands of the units read
SELECT rows_equal, offsets_decoded < rows_scanned / 10 AS only_the_pages_with_a_row, rows_scanned > 10000 AS units_are_large
FROM vl_scan('SELECT foreign_id_b, avg(score) FROM vl WHERE dim_a_id = 90 AND d >= ''2023-03-01'' AND d < ''2023-04-01'' GROUP BY 1');

-- the same after a VACUUM and an UPDATE of a few rows: only the pages the UPDATE touched are not all-visible
VACUUM vl;
UPDATE vl SET score = score + 1 WHERE id % 5000 = 0;
SELECT rows_equal, offsets_decoded < rows_scanned / 10 AS only_the_pages_with_a_row
FROM vl_scan('SELECT foreign_id_b, avg(score) FROM vl WHERE dim_a_id = 90 AND d >= ''2023-03-01'' AND d < ''2023-04-01'' GROUP BY 1');

DROP FUNCTION vl_scan(text);
DROP TABLE vl;
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
