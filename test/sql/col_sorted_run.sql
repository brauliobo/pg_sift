-- The sorted run of a leaf in every unit (column format minor 13, docs/COL.md 2.14): an index with the option order_columns keeps, per unit and leaf, the
-- ordinals of the rows in the order of the column, so that ORDER BY .. LIMIT reads the first rows of each unit and not all of them. Written only under
-- sift.col_format_minor = 13 and for the leaves of the option, so that an older library keeps reading what an index without it, or under a lower
-- setting, writes; read back by the decoder that refuses a damaged run; recomputed by sift.col_verify; kept by promotion, merge and VACUUM.
-- The rows and their order are those of the plain plan: every direction, NULLS FIRST and LAST, ties, NULLs, NaN and infinities, filters, OFFSET, the
-- write buffer, deletes and updates, workers.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET statement_timeout = '120s';

-- 30,000 rows in about 24 units. k: 500 values (about 60 rows each), kn: 40 values and a NULL in 7 rows, t: ties, NULLs and infinity, f: NaN, -0, 0 and NULLs,
-- n: a numeric with NaN (stored as an overflow) and NULLs, d: a date; c: ascending in runs of 100 (clustered), cd: descending; sk: its best values are
-- all in the first units, so that the first pass of a top-N leaves those unfinished
CREATE TABLE sr (id int, k int, kn int, t timestamptz, f float8, n numeric(12, 2), d date, c int, cd int, sk int, pad text) WITH (autovacuum_enabled = off);
ALTER TABLE sr ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO sr
SELECT g, (hashint4(g) & 2147483647) % 500,
       CASE WHEN g % 7 = 0 THEN NULL ELSE (hashint4(g + 1) & 2147483647) % 40 END,
       CASE WHEN g % 11 = 0 THEN NULL WHEN g % 997 = 0 THEN 'infinity'::timestamptz
            ELSE timestamptz '2024-01-01 00:00:00+00' + ((hashint4(g + 2) & 2147483647) % 20000) * interval '1 minute' END,
       CASE WHEN g % 13 = 0 THEN NULL WHEN g % 101 = 0 THEN 'NaN'::float8 WHEN g % 103 = 0 THEN '-0'::float8 WHEN g % 107 = 0 THEN 0::float8
            ELSE ((hashint4(g + 3) & 2147483647) % 1000) / 8.0 - 60 END,
       CASE WHEN g % 17 = 0 THEN NULL WHEN g % 211 = 0 THEN 'NaN'::numeric ELSE ((hashint4(g + 4) & 2147483647) % 100000) / 100.0 END,
       CASE WHEN g % 19 = 0 THEN NULL ELSE date '2020-01-01' + (hashint4(g + 5) & 2147483647) % 2000 END,
       g / 100, 100000 - g,
       CASE WHEN g <= 3000 THEN (hashint4(g + 6) & 2147483647) % 1000 ELSE 1000 + (hashint4(g + 6) & 2147483647) % 100000 END, repeat('x', 1500)
FROM generate_series(1, 30000) g;

-- the minor of the segments: 4 (this schema has no jsonb paths) without the option or under a lower setting, 13 with both, and the option writes nothing else
CREATE TABLE sr_o (LIKE sr) WITH (autovacuum_enabled = off);
ALTER TABLE sr_o ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO sr_o SELECT * FROM sr ORDER BY id;
CREATE TABLE sr_t (LIKE sr) WITH (autovacuum_enabled = off);
ALTER TABLE sr_t ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO sr_t SELECT * FROM sr ORDER BY id;
CREATE INDEX sr_off ON sr_o USING sift_col (id, k, kn, t, f, n, d, c, cd, sk) WITH (write_buffer_limit = 4096, cube_minmax = false, superunit = 0);
SET sift.col_format_minor = 12;
CREATE INDEX sr_twelve ON sr_t USING sift_col (id, k, kn, t, f, n, d, c, cd, sk) WITH (write_buffer_limit = 4096, order_columns = 'k, kn, t, f, n, d, c, cd, sk', cube_minmax = false, superunit = 0);
RESET sift.col_format_minor;
CREATE INDEX sr_on ON sr USING sift_col (id, k, kn, t, f, n, d, c, cd, sk) WITH (write_buffer_limit = 4096, order_columns = 'id, k, kn, t, f, n, d, c, cd, sk', cube_minmax = false, superunit = 0);
SELECT 'sr_off' AS idx, array_agg(DISTINCT format) AS minors FROM sift._test_col_formats('sr_off')
UNION ALL SELECT 'sr_twelve', array_agg(DISTINCT format) FROM sift._test_col_formats('sr_twelve')
UNION ALL SELECT 'sr_on', array_agg(DISTINCT format) FROM sift._test_col_formats('sr_on') ORDER BY 1;
SELECT sift.col_verify('sr_off', true), sift.col_verify('sr_twelve', true), sift.col_verify('sr_on', true);
SELECT pg_relation_size('sr_twelve') = pg_relation_size('sr_off') AS lower_is_off_size,
       (SELECT array_agg(section || coalesce(':' || leaf, '') || '=' || bytes ORDER BY section, leaf) FROM sift.col_sections('sr_twelve'))
       = (SELECT array_agg(section || coalesce(':' || leaf, '') || '=' || bytes ORDER BY section, leaf) FROM sift.col_sections('sr_off')) AS lower_is_off_sections;
SELECT count(*) FILTER (WHERE section = 'runs') AS rows_off FROM sift.col_sections('sr_off');
-- two bytes a row for each of the 8 leaves whose rows do not ascend with the column (k, kn, t, f, n, d, cd, sk), a header for the 2 that do (id, c)
SELECT bytes > 0 AS runs_take_bytes, bytes BETWEEN 8 * 2 * 30000 AND 8 * 2 * 30000 * 1.02 AS two_bytes_a_row_per_unordered_leaf FROM sift.col_sections('sr_on') WHERE section = 'runs';

-- the options are checked: names, types that are not ordered by their key (text, uuid, bool), a leaf of a jsonb path
CREATE INDEX sr_bad1 ON sr USING sift_col (id, k) WITH (order_columns = 'nosuch', cube_minmax = false, superunit = 0);
CREATE INDEX sr_bad2 ON sr USING sift_col (id, k, kn) WITH (order_columns = 'k,,kn', cube_minmax = false, superunit = 0);
CREATE TABLE sr_x (id int, s text, b bool, u uuid, j jsonb);
CREATE INDEX sr_bad3 ON sr_x USING sift_col (id, s) WITH (order_columns = 's', cube_minmax = false, superunit = 0);
CREATE INDEX sr_bad4 ON sr_x USING sift_col (id, b) WITH (order_columns = 'b', cube_minmax = false, superunit = 0);
CREATE INDEX sr_bad5 ON sr_x USING sift_col (id, u) WITH (order_columns = 'u', cube_minmax = false, superunit = 0);
DROP TABLE sr_x;

-- the plain plan, the top-N scan with the runs, and the top-N scan without them (sift.topn_runs = off)
CREATE FUNCTION sr_mode(m text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.enable_agg', (m <> 'plain')::text, false);
	PERFORM set_config('sift.topn_runs', (m = 'runs')::text, false);
	PERFORM set_config('enable_seqscan', (m = 'plain')::text, false);
	PERFORM set_config('enable_indexscan', (m = 'plain')::text, false);
	PERFORM set_config('enable_bitmapscan', (m = 'plain')::text, false);
END $$;

-- the rows as text, in order; -0 and 0 are the same value and either may be the row a plan takes among ties
CREATE FUNCTION sr_rows(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE 'SELECT md5(string_agg(r, ''|'' ORDER BY pos)) FROM (SELECT row_number() OVER () AS pos, regexp_replace(row_to_json(x)::text, ''-0(?![.0-9])'', ''0'', ''g'') AS r FROM (' || q || ') x) y' INTO r;
	RETURN r;
END $$;

-- the units a query read through their sorted run (0 when the plan is not a top-N scan or reads none)
CREATE FUNCTION sr_runs(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, TIMING OFF, COSTS OFF, FORMAT JSON) ' || q INTO j;
	RETURN coalesce(jsonb_path_query_first(j, '$.**."Units Read By Sorted Run"')::int, 0);
END $$;

-- one query three ways: the rows (with the key and the id as the order, so that the rows are the same) and, for the key alone, its values
CREATE FUNCTION sr_check(tab text, cols text[], lims int[], OUT n int, OUT shape text, OUT same bool, OUT runs int) RETURNS SETOF record LANGUAGE plpgsql AS $$
DECLARE
	col text; dir text; nl text; lim int; flt text; q text; qk text; a text; b text; c text; ak text; bk text; ck text; i int := 0;
BEGIN
	FOREACH col IN ARRAY cols LOOP
	FOREACH dir IN ARRAY ARRAY['ASC', 'DESC'] LOOP
	FOREACH nl IN ARRAY ARRAY['NULLS FIRST', 'NULLS LAST'] LOOP
	FOREACH lim IN ARRAY lims LOOP
	FOREACH flt IN ARRAY ARRAY['', 'WHERE id % 7 = 3'] LOOP
		i := i + 1;
		q := format('SELECT id, %1$s FROM %2$s %3$s ORDER BY %1$s %4$s %5$s, id LIMIT %6$s%7$s', col, tab, flt, dir, nl, lim, CASE WHEN lim = 300 THEN ' OFFSET 25' ELSE '' END);
		qk := format('SELECT %1$s FROM %2$s %3$s ORDER BY %1$s %4$s %5$s LIMIT %6$s', col, tab, flt, dir, nl, lim);
		PERFORM sr_mode('plain'); a := sr_rows(q); ak := sr_rows(qk);
		PERFORM sr_mode('norun'); b := sr_rows(q); bk := sr_rows(qk);
		PERFORM sr_mode('runs'); c := sr_rows(q); ck := sr_rows(qk);
		n := i; shape := format('%s %s %s limit %s %s', col, dir, nl, lim, flt);
		same := a = b AND b = c;
		-- a key alone ties: any rows of the tied values are the plain plan's, and the values are
		same := same AND ak = bk AND bk = ck;
		runs := sr_runs(qk);
		RETURN NEXT;
	END LOOP; END LOOP; END LOOP; END LOOP; END LOOP;
	PERFORM sr_mode('runs');
END $$;

CREATE TEMP TABLE sr_res AS SELECT * FROM sr_check('sr', ARRAY['k', 'kn', 't', 'f', 'n', 'd', 'c', 'cd', 'id', 'sk'], ARRAY[1, 300, 4000]);
SELECT count(*) AS queries, count(*) FILTER (WHERE same) AS same, count(*) FILTER (WHERE runs > 0) AS read_runs FROM sr_res;
SELECT n, shape FROM sr_res WHERE NOT same;
-- the same rows from an index without runs, and from one written under a lower setting, which read none
SELECT count(*) AS queries, count(*) FILTER (WHERE same) AS same, count(*) FILTER (WHERE runs > 0) AS read_runs FROM sr_check('sr_o', ARRAY['kn', 't'], ARRAY[300]);
SELECT count(*) AS queries, count(*) FILTER (WHERE same) AS same, count(*) FILTER (WHERE runs > 0) AS read_runs FROM sr_check('sr_t', ARRAY['kn', 't'], ARRAY[300]);
-- the superunit summaries (minor 12) and the runs (minor 13) in one segment: the directory carries both, and verify and the plans agree
CREATE TABLE sr_s (LIKE sr) WITH (autovacuum_enabled = off);
ALTER TABLE sr_s ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO sr_s SELECT * FROM sr ORDER BY id;
CREATE INDEX sr_both ON sr_s USING sift_col (id, k, kn, t, f, n, d, c, cd, sk) WITH (write_buffer_limit = 4096, order_columns = 'k, kn, f, sk', superunit = 3, cube_minmax = false);
SELECT array_agg(DISTINCT format) AS minors, sift.col_verify('sr_both', true) FROM sift._test_col_formats('sr_both');
SELECT count(*) AS queries, count(*) FILTER (WHERE same) AS same, count(*) FILTER (WHERE runs > 0) AS read_runs FROM sr_check('sr_s', ARRAY['k', 'f'], ARRAY[1, 300]);
SELECT count(*) AS sums_agree FROM (SELECT sum(c) AS a, sum(k) AS b FROM sr_s) x, (SELECT sum(c) AS a, sum(k) AS b FROM sr) y WHERE x.a = y.a AND x.b = y.b;

-- the units a query reads through the runs: the first rows of each and not every row; none with the setting off or on an index without them
SELECT sr_runs('SELECT id, k FROM sr ORDER BY k LIMIT 10') > 0 AS runs_read, sr_runs('SELECT id, k FROM sr ORDER BY k DESC LIMIT 10') > 0 AS runs_read_desc,
       sr_runs('SELECT id, k FROM sr_o ORDER BY k LIMIT 10') AS none_without_runs;
SET sift.topn_runs = off;
SELECT sr_runs('SELECT id, k FROM sr ORDER BY k LIMIT 10') AS none_with_setting_off;
RESET sift.topn_runs;

-- a first pass that leaves units unfinished: sk has all its best values in three units, which the second pass reads again (more units read than there are)
SELECT sr_runs('SELECT id, sk FROM sr ORDER BY sk LIMIT 300') > sr_runs('SELECT id, k FROM sr ORDER BY k LIMIT 300') AS second_pass_reads_units;

CREATE FUNCTION sr_small(tab text) RETURNS TABLE (queries bigint, same bigint, read_runs bigint, different text[]) LANGUAGE sql AS $$
	SELECT count(*), count(*) FILTER (WHERE same), count(*) FILTER (WHERE runs > 0), array_agg(shape) FILTER (WHERE NOT same) FROM sr_check(tab, ARRAY['k', 'kn', 't', 'f', 'c', 'sk'], ARRAY[1, 300])
$$;

-- rows in the write buffer have no run and are read whatever the runs say; deletes and updates leave the rows of a run that are not live, and the new
-- version of an updated row is in the buffer; VACUUM, promotion and merge write the runs again
INSERT INTO sr SELECT g, (hashint4(g) & 2147483647) % 500, CASE WHEN g % 2 = 0 THEN NULL ELSE g % 3 END, timestamptz '2023-06-01' + g * interval '1 second',
       CASE WHEN g % 5 = 0 THEN NULL ELSE -g END, 1.5, date '2019-01-01', g / 100, 100000 - g, repeat('x', 1500) FROM generate_series(30001, 30600) g;
SELECT * FROM sr_small('sr');
UPDATE sr SET k = 2, kn = NULL, t = NULL, f = 'NaN' WHERE id % 1000 = 5;
UPDATE sr SET k = 1000 + k WHERE id % 1000 = 6;
DELETE FROM sr WHERE id % 7 = 0;
SELECT * FROM sr_small('sr');
VACUUM (freeze, index_cleanup on, disable_page_skipping) sr;
SELECT * FROM sr_small('sr');
SELECT sift.col_promote('sr_on') >= 0 AS promoted;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('sr_on');
SELECT sift.col_verify('sr_on', true);
SELECT * FROM sr_small('sr');
SELECT sift.col_merge('sr_on', true) >= 0 AS merged;
VACUUM (freeze, index_cleanup on, disable_page_skipping) sr;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors, count(*) AS segments FROM sift._test_col_formats('sr_on');
SELECT sift.col_verify('sr_on', true);
SELECT * FROM sr_small('sr');

-- with workers: the rows are the plain plan's
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SELECT * FROM sr_small('sr');
RESET max_parallel_workers_per_gather;

-- the runs are decoded with checks: a cut run, a flag bit, an unknown kind, a row count that is not the unit's, counts that do not fit, a width that is not
-- the one the row count says are refused; an ordinal that is no row of the unit is refused when the entries are read; a changed byte that is accepted is
-- another valid run, and the comparison with the chunk (what sift.col_verify does) refuses it
SELECT blobs > 0 AS has_blobs, ord > 0 AS has_ord, ident > 0 AS has_ident, intact = blobs AS intact_decodes, matching = blobs AS intact_match_chunks,
       cut > 0 AS cuts_made, cut_accepted = 0 AS cuts_refused, named > 0 AS named_made, named_accepted = 0 AS named_refused,
       damaged > 0 AS damages_made, accepted < damaged AS some_refused, missed = 0 AS accepted_differ_from_chunk
FROM sift._test_col_run_damage('sr_on', 12);
-- the bytes of the section as the segment holds them, one bit flipped at a time (the header, the records, the directories and the head of every run, then one entry in 997): every
-- one in a byte that is used is refused by the reader or by the check
CREATE TABLE sr_f (id int, k int, f float8, n numeric(12, 2), pad text) WITH (autovacuum_enabled = off);
ALTER TABLE sr_f ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO sr_f SELECT g, (hashint4(g) & 2147483647) % 300, g % 91 * 0.25, g % 17 * 1.5, repeat('f', 1500) FROM generate_series(1, 4500) g;
CREATE INDEX sr_f_idx ON sr_f USING sift_col (id, k, f, n) WITH (write_buffer_limit = 4096, order_columns = 'id, k, f, n', cube_minmax = false, superunit = 0);
SELECT runs > 0 AS has_runs, damaged > 1000 AS many, refused > 0 AS some_refused, unnoticed FROM sift._test_col_run_file_damage('sr_f_idx', 997);

-- an index of the old kind takes the option later: the units written before keep their minor and have no run, the ones after are minor 13, a merge writes
-- one segment of minor 13 and REINDEX gives every unit its run; the option switched off, the next merge writes the old minor; under a lower setting
-- nothing of it is written
CREATE TABLE sa (id int, k int, t timestamptz, pad text) WITH (autovacuum_enabled = off);
ALTER TABLE sa ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO sa SELECT g, (hashint4(g) & 2147483647) % 500, timestamptz '2024-01-01' + ((hashint4(g + 2) & 2147483647) % 20000) * interval '1 minute', repeat('x', 1500) FROM generate_series(1, 12000) g;
CREATE INDEX sa_idx ON sa USING sift_col (id, k, t) WITH (write_buffer_limit = 4096, cube_minmax = false, superunit = 0);
SELECT array_agg(DISTINCT format) AS minors FROM sift._test_col_formats('sa_idx');
ALTER INDEX sa_idx SET (order_columns = 'k, t');
INSERT INTO sa SELECT g, (hashint4(g) & 2147483647) % 500, timestamptz '2024-01-01' + ((hashint4(g + 2) & 2147483647) % 20000) * interval '1 minute', repeat('x', 1500) FROM generate_series(12001, 24000) g;
SELECT sift.col_promote('sa_idx') >= 0 AS promoted;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('sa_idx');
SELECT sift.col_verify('sa_idx', true);
SELECT * FROM sr_check('sa', ARRAY['k', 't'], ARRAY[1, 300]) LIMIT 0;
SELECT count(*) AS queries, count(*) FILTER (WHERE same) AS same, count(*) FILTER (WHERE runs > 0) AS read_runs FROM sr_check('sa', ARRAY['k', 't'], ARRAY[1, 300]);
SELECT sift.col_merge('sa_idx', true) >= 0 AS merged;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors, count(*) AS segments FROM sift._test_col_formats('sa_idx');
SELECT blobs > 0 AS has_runs FROM sift._test_col_run_damage('sa_idx', 3);
SELECT sift.col_verify('sa_idx', true);
ALTER INDEX sa_idx SET (order_columns = '');
INSERT INTO sa SELECT g, g % 500, timestamptz '2024-01-01' + g * interval '1 minute', repeat('x', 1500) FROM generate_series(24001, 28000) g;
SELECT sift.col_promote('sa_idx') >= 0 AS promoted;
SELECT sift.col_merge('sa_idx', true) >= 0 AS merged;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('sa_idx');
SELECT blobs AS runs FROM sift._test_col_run_damage('sa_idx', 3);
SELECT sift.col_verify('sa_idx', true);
ALTER INDEX sa_idx SET (order_columns = 'k, t');
SET sift.col_format_minor = 6;
INSERT INTO sa SELECT g, g % 500, timestamptz '2024-01-01' + g * interval '1 minute', repeat('x', 1500) FROM generate_series(28001, 30000) g;
SELECT sift.col_promote('sa_idx') >= 0 AS promoted;
SELECT sift.col_merge('sa_idx', true) >= 0 AS merged;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('sa_idx');
RESET sift.col_format_minor;
SELECT sift.col_verify('sa_idx', true);
REINDEX INDEX sa_idx;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('sa_idx');
SELECT blobs > 0 AS has_runs FROM sift._test_col_run_damage('sa_idx', 3);
SELECT sift.col_verify('sa_idx', true);
SELECT count(*) AS queries, count(*) FILTER (WHERE same) AS same, count(*) FILTER (WHERE runs > 0) AS read_runs FROM sr_check('sa', ARRAY['k', 't'], ARRAY[1, 300]);

DROP TABLE sa, sr, sr_o, sr_t, sr_s, sr_f CASCADE;
DROP FUNCTION sr_mode(text), sr_rows(text), sr_runs(text), sr_check(text, text[], int[]), sr_small(text);
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
