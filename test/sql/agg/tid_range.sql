-- ctid range predicates in the aggregate scan (docs/AGG.md, "ctid ranges"): the scan reads the parts and granules with a row on a block of the range and tests
-- the TIDs of the rows of its edges. Every statement is run with the pushdown and with sift.enable_agg = off: the rows must be the same, after every kind of write.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
-- Format 2 (with the test library the index and the table are Format 2 on every run); agg/tid_range_f1 runs the same statements on Format 1, whose units are superblocks.
SELECT current_setting('sift.test_create_format1', true) IS NOT NULL AND current_setting('sift.test_agg_par_min_rows', true) IS NOT NULL AS testing \gset
\if :testing
SET sift.test_create_format1 = off;
\endif
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;
SET enable_tidscan = off;

-- rows of 500 bytes at a fill factor that leaves room for HOT updates: 9 rows a page, one granule (8192 rows)
CREATE TABLE th (id int, g int, v int, pad text) WITH (autovacuum_enabled = false, fillfactor = 60);
INSERT INTO th SELECT i, i % 7, i % 100, repeat('x', 500) FROM generate_series(1, 6000) i;
CREATE INDEX th_col ON th USING sift_col (id, g, v);
VACUUM (FREEZE, ANALYZE) th;
SELECT max(ctid) AS last_tid, count(DISTINCT (ctid::text::point)[0]::int / 256) AS superblocks FROM th;

-- a sift_table: about 290 rows a block, four granules
CREATE TABLE tt (id int, g int, v int, pad text) USING sift_table;
INSERT INTO tt SELECT i, i % 7, i % 100, repeat('x', 100) FROM generate_series(1, 30000) i;
SELECT max(ctid) AS last_tid, count(DISTINCT (ctid::text::point)[0]::int / 256) AS superblocks FROM tt;

-- narrow rows: about 220 pages, four granules of 8192 rows in the order of their TIDs; gb is the block of the last row of the first granule
CREATE TABLE tg (id int, g int, v int) WITH (autovacuum_enabled = false);
INSERT INTO tg SELECT i, i % 7, i % 100 FROM generate_series(1, 30000) i;
CREATE INDEX tg_col ON tg USING sift_col (id, g, v);
VACUUM (FREEZE, ANALYZE) tg;
SELECT (ctid::text::point)[0]::int AS gb FROM tg WHERE id = 8192 \gset

CREATE FUNCTION plan_of(q text) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.plan_siftagg_with_workers_label(q) $$;

CREATE FUNCTION same(q text) RETURNS TABLE (plan text, result_rows bigint, vs_normal_plan bigint) LANGUAGE sql AS $$ SELECT * FROM sift_helpers.agg_plan_and_differences(plan_of(q), q) $$;

-- the ranges, on a table: inside one block, across blocks, across a superblock boundary, bounds past the end, empty, crossed, strict, equal to one TID,
-- an offset of 0 and the largest one, the largest TID there is
CREATE TABLE tr_range (n int, lo text, hi text);
INSERT INTO tr_range VALUES
	(1, '(100,3)', '(100,7)'), (2, '(10,4)', '(300,2)'), (3, '(255,5)', '(256,3)'), (4, '(255,1)', '(256,1)'), (5, '(500,1)', '(100000,1)'),
	(6, '(0,1)', '(2,5)'), (7, '(20,0)', '(30,65535)'), (8, '(256,0)', '(511,65535)'), (9, '(300,6)', '(300,6)'), (10, '(300,6)', '(300,5)'),
	(11, '(0,0)', '(4294967295,65535)'), (12, '(600,1)', '(4294967295,65535)'), (13, '(4294967295,65535)', '(4294967295,65535)');

-- both ends included, both ends excluded, one end only, the constants on the left, BETWEEN, a TID with an offset no row has
CREATE FUNCTION th_q(n int, form int) RETURNS text LANGUAGE sql AS $$
	SELECT format(CASE form
		WHEN 1 THEN 'SELECT g, count(*) AS n, sum(v) AS s, min(id) AS lo, max(id) AS hi FROM %%I WHERE ctid >= %L AND ctid <= %L GROUP BY g'
		WHEN 2 THEN 'SELECT count(*) AS n, sum(v) AS s, min(id) AS lo, max(id) AS hi FROM %%I WHERE ctid > %L AND ctid < %L'
		WHEN 3 THEN 'SELECT count(*) AS n, sum(v) AS s FROM %%I WHERE ctid >= %L'
		WHEN 4 THEN 'SELECT count(*) AS n, sum(v) AS s FROM %%I WHERE ctid <= %L'
		WHEN 5 THEN 'SELECT count(*) AS n, sum(v) AS s FROM %%I WHERE %L <= ctid AND %L > ctid'
		WHEN 6 THEN 'SELECT count(*) AS n, count(DISTINCT g) AS gs FROM %%I WHERE ctid BETWEEN %L AND %L'
	END, lo, hi) FROM tr_range WHERE tr_range.n = th_q.n $$;

CREATE FUNCTION all_ranges(tbl text) RETURNS TABLE (n int, form int, plan text, result_rows bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
BEGIN
	FOR n IN SELECT r.n FROM tr_range r ORDER BY 1 LOOP
		FOR form IN 1..6 LOOP
			SELECT s.plan, s.result_rows, s.vs_normal_plan INTO plan, result_rows, vs_normal_plan FROM same(format(th_q(n, form), tbl)) s;
			RETURN NEXT;
		END LOOP;
	END LOOP;
END $$;

-- the row counts the ranges hold, from the normal plan
SET sift.enable_agg = off;
SELECT n, (SELECT count(*) FROM th WHERE ctid >= lo::tid AND ctid <= hi::tid) AS rows_in_th FROM tr_range ORDER BY n;
RESET sift.enable_agg;

-- all the ranges, all the forms: the plan is SiftAgg, the result is the normal plan's
SELECT count(*) AS statements, count(*) FILTER (WHERE plan LIKE 'SiftAgg%') AS scanned, sum(vs_normal_plan) AS differing_rows,
       array_agg(n || '/' || form) FILTER (WHERE plan NOT LIKE 'SiftAgg%' OR vs_normal_plan <> 0) AS failing FROM all_ranges('th');

-- the plan shows the range, and the scan reads the granules it touches only
SELECT plan_of('SELECT count(*) FROM th WHERE ctid >= ''(10,1)'' AND ctid < ''(20,1)''') AS range,
       plan_of('SELECT count(*) FROM th WHERE NOT (ctid >= ''(10,1)'')') AS negated_range;
-- the granules the SiftAgg node read or answered from their pre-aggregates, not those the range ruled out (NULL: no SiftAgg)
CREATE FUNCTION units_of(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; n int;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ 'SiftAgg' AND n IS NULL THEN n := 0; END IF;
		IF l ~ '^\s*Granules: ' THEN
			n := n + (SELECT sum(m[1]::int) FROM regexp_matches(l, '(?:scanned|preagg)=(\d+)', 'g') m);
		END IF;
	END LOOP;
	RETURN n;
END $$;
SELECT units_of('SELECT count(*), sum(v) FROM tg') AS all_units,
       units_of(format('SELECT count(*), sum(v) FROM tg WHERE ctid >= ''(%s,1)'' AND ctid < ''(%s,1)''', :gb - 20, :gb - 10)) AS one_granule,
       units_of(format('SELECT count(*), sum(v) FROM tg WHERE ctid >= ''(%s,1)'' AND ctid < ''(%s,1)''', :gb - 2, :gb + 2)) AS two_granules,
       units_of('SELECT count(*), sum(v) FROM tg WHERE ctid >= ''(100000,1)''') AS beyond_the_end,
       units_of('SELECT count(*), sum(v) FROM tg WHERE ctid > ''(5,3)'' AND ctid < ''(5,3)''') AS empty_range;
SELECT * FROM same(format('SELECT g, count(*) AS n, sum(v) AS s FROM tg WHERE ctid >= ''(%s,1)'' AND ctid < ''(%s,1)'' GROUP BY g', :gb - 2, :gb + 2));

-- HOT updates of a column the index does not hold: the row keeps the TID of the root of its chain in the index, its ctid is the new version's, on the
-- same page; the edge blocks of a range are the ones that need the version
CREATE TEMP TABLE th_before AS SELECT id, ctid AS tid FROM th;
UPDATE th SET pad = repeat('y', 500) WHERE id % 5 = 0;
SELECT count(*) > 0 AS hot_versions_moved_on_their_page
FROM th JOIN th_before USING (id) WHERE th.ctid <> th_before.tid AND (th.ctid::text::point)[0] = (th_before.tid::text::point)[0];
SELECT count(*) AS statements, count(*) FILTER (WHERE plan LIKE 'SiftAgg%') AS scanned, sum(vs_normal_plan) AS differing_rows,
       array_agg(n || '/' || form) FILTER (WHERE plan NOT LIKE 'SiftAgg%' OR vs_normal_plan <> 0) AS failing FROM all_ranges('th');
-- a range that ends at the offset of a new version
SELECT * FROM same(format('SELECT count(*) AS n, sum(v) AS s FROM th WHERE ctid >= %L AND ctid <= %L', (SELECT min(ctid) FROM th WHERE id % 5 = 0 AND ctid > '(100,0)'), (SELECT max(ctid) FROM th WHERE id % 5 = 0 AND ctid < '(200,0)')));
-- the same rows updated again: chains of two versions, then pruned by a VACUUM
UPDATE th SET pad = repeat('z', 500) WHERE id % 5 = 0 AND id % 2 = 0;
SELECT count(*) AS statements, count(*) FILTER (WHERE plan LIKE 'SiftAgg%') AS scanned, sum(vs_normal_plan) AS differing_rows FROM all_ranges('th');
VACUUM th;
SELECT count(*) AS statements, count(*) FILTER (WHERE plan LIKE 'SiftAgg%') AS scanned, sum(vs_normal_plan) AS differing_rows FROM all_ranges('th');

-- the versions of this transaction, then of a rolled back one
BEGIN;
UPDATE th SET pad = repeat('w', 500) WHERE id % 3 = 0;
UPDATE th SET v = v + 1 WHERE id % 13 = 0;
DELETE FROM th WHERE id % 17 = 0;
SELECT count(*) AS statements, count(*) FILTER (WHERE plan LIKE 'SiftAgg%') AS scanned, sum(vs_normal_plan) AS differing_rows FROM all_ranges('th');
ROLLBACK;
SELECT count(*) AS statements, count(*) FILTER (WHERE plan LIKE 'SiftAgg%') AS scanned, sum(vs_normal_plan) AS differing_rows FROM all_ranges('th');

-- updates of indexed columns (new TIDs), deletes, VACUUM, and new rows on the TIDs that VACUUM freed
UPDATE th SET v = v + 1 WHERE id % 7 = 0;
DELETE FROM th WHERE id % 11 = 0;
SELECT count(*) AS statements, count(*) FILTER (WHERE plan LIKE 'SiftAgg%') AS scanned, sum(vs_normal_plan) AS differing_rows FROM all_ranges('th');
VACUUM th;
INSERT INTO th SELECT i, i % 7, i % 100, repeat('n', 500) FROM generate_series(6001, 7500) i;
SELECT count(*) AS statements, count(*) FILTER (WHERE plan LIKE 'SiftAgg%') AS scanned, sum(vs_normal_plan) AS differing_rows FROM all_ranges('th');
VACUUM (FREEZE) th;
SELECT count(*) AS statements, count(*) FILTER (WHERE plan LIKE 'SiftAgg%') AS scanned, sum(vs_normal_plan) AS differing_rows FROM all_ranges('th');

-- a sift_table: the TID is the engine's, written rows and deleted rows included
SELECT count(*) AS statements, count(*) FILTER (WHERE plan LIKE 'SiftAgg%') AS scanned, sum(vs_normal_plan) AS differing_rows FROM all_ranges('tt');
UPDATE tt SET v = v + 1 WHERE id % 7 = 0;
DELETE FROM tt WHERE id % 11 = 0;
INSERT INTO tt SELECT i, i % 7, i % 100, repeat('n', 100) FROM generate_series(30001, 31000) i;
SELECT count(*) AS statements, count(*) FILTER (WHERE plan LIKE 'SiftAgg%') AS scanned, sum(vs_normal_plan) AS differing_rows,
       array_agg(n || '/' || form) FILTER (WHERE plan NOT LIKE 'SiftAgg%' OR vs_normal_plan <> 0) AS failing FROM all_ranges('tt');
SELECT * FROM same('SELECT g, count(*) AS n, sum(v) AS s FROM tt WHERE ctid >= ''(50,30)'' AND ctid < ''(52,10)'' AND v < 60 GROUP BY g');
SELECT * FROM same('SELECT g, count(*) AS n, sum(v) AS s FROM tt WHERE ctid >= (SELECT min(ctid) FROM tt WHERE id > 20000) GROUP BY g');

-- parallel workers take the granules of the range (the test library plans them for tables this small; the release library as its cost says)
\set with_workers 'SiftAgg%'
\if :testing
SET sift.test_agg_par_min_rows = 0;
\set with_workers 'SiftAgg with workers'
\endif
SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0; SET min_parallel_index_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SELECT plan LIKE :'with_workers' AS planned, count(*) AS statements, sum(vs_normal_plan) AS differing_rows FROM all_ranges('th') GROUP BY 1;
SELECT plan LIKE :'with_workers' AS planned, count(*) AS statements, sum(vs_normal_plan) AS differing_rows FROM all_ranges('tt') GROUP BY 1;
\if :testing
RESET sift.test_agg_par_min_rows;
\endif
-- as the cost says, with launches that cost nothing: a range of one granule takes no workers, the whole table does. The planner reads a ctid range of a sift_table by the TIDs of its
-- granules, not by the pages of the relation
CREATE TABLE tu (id int, g int, v int, pad text) USING sift_table;
INSERT INTO tu SELECT i, i % 7, i % 100, repeat('x', 100) FROM generate_series(1, 30000) i;
VACUUM (FREEZE, ANALYZE) tu;
SELECT t, plan_of(format('SELECT count(*), sum(v) FROM %I WHERE ctid BETWEEN ''(0,1)'' AND ''(2,5)''', t)) AS first_blocks,
       plan_of(format('SELECT count(*), sum(v) FROM %I WHERE ctid >= ''(100000,1)''', t)) AS beyond_the_end,
       plan_of(format('SELECT count(*), sum(v) FROM %I WHERE ctid >= ''(0,1)'' AND ctid <= ''(4294967295,65535)''', t)) AS whole_table
FROM (VALUES ('tg'), ('tu')) v(t) ORDER BY t;
SET max_parallel_workers_per_gather = 0;

-- what stays on the normal plan: <>, ctid in an expression, an OR, a list, a column of another table, a partitioned table, a parameter of a generic plan
SELECT plan_of('SELECT count(*) FROM th WHERE ctid <> ''(3,2)''') AS ne,
       plan_of('SELECT count(*) FROM th WHERE ctid::text LIKE ''(1%''') AS expr,
       plan_of('SELECT count(*) FROM th WHERE ctid < ''(10,1)'' OR g = 3') AS or_,
       plan_of('SELECT count(*) FROM th WHERE ctid = ANY (ARRAY[''(1,1)''::tid, ''(2,2)''])') AS list,
       plan_of('SELECT count(*) FILTER (WHERE ctid > ''(10,1)'') FROM th') AS filter_,
       plan_of('SELECT ctid::text, count(*) FROM th WHERE ctid < ''(2,1)'' GROUP BY ctid') AS grouped,
       plan_of('SELECT count(*) FROM th WHERE ctid >= (''('' || (id % 50) || '',1)'')::tid') AS column_;
SELECT * FROM same('SELECT count(*) AS n, sum(v) AS s FROM th WHERE ctid <> ''(3,2)''');
SELECT * FROM same('SELECT count(*) AS n, sum(v) AS s FROM th WHERE ctid < ''(10,1)'' OR g = 3');
SET plan_cache_mode = force_generic_plan;
PREPARE th_generic (tid, tid) AS SELECT g, count(*) AS n FROM th WHERE ctid >= $1 AND ctid < $2 GROUP BY g;
SELECT plan_of('EXECUTE th_generic(''(10,1)'', ''(300,1)'')');
SET plan_cache_mode = force_custom_plan;
PREPARE th_custom (tid, tid) AS SELECT g, count(*) AS n FROM th WHERE ctid >= $1 AND ctid < $2 GROUP BY g ORDER BY g;
SELECT plan_of('EXECUTE th_custom(''(10,1)'', ''(300,1)'')');
EXECUTE th_custom('(10,1)', '(300,1)');
RESET plan_cache_mode;
DEALLOCATE th_generic;
DEALLOCATE th_custom;

-- the TIDs of a partitioned table are those of each partition: the normal plan
CREATE TABLE tp (id int, g int) PARTITION BY RANGE (id);
CREATE TABLE tp_a PARTITION OF tp FOR VALUES FROM (0) TO (1000) WITH (autovacuum_enabled = false);
CREATE TABLE tp_b PARTITION OF tp FOR VALUES FROM (1000) TO (2000) WITH (autovacuum_enabled = false);
INSERT INTO tp SELECT i, i % 5 FROM generate_series(0, 1999) i;
CREATE INDEX tp_col ON tp USING sift_col (id, g);
SELECT plan_of('SELECT g, count(*) FROM tp GROUP BY g') AS no_range, plan_of('SELECT g, count(*) FROM tp WHERE ctid >= ''(1,1)'' GROUP BY g') AS with_range;
SELECT * FROM same('SELECT g, count(*) AS n FROM tp WHERE ctid >= ''(1,1)'' GROUP BY g');

-- the fact of a join to a dimension: its scan keeps the rows of the range, on an index and on a sift_table
CREATE TABLE th_dim (g int PRIMARY KEY, label text);
INSERT INTO th_dim SELECT g, 'g' || g FROM generate_series(0, 6) g;
ANALYZE th_dim;
SELECT * FROM same('SELECT d.label, count(*) AS n, sum(f.v) AS s FROM th f JOIN th_dim d ON d.g = f.g WHERE f.ctid >= ''(10,4)'' AND f.ctid < ''(300,2)'' AND d.g <> 3 GROUP BY d.label');
SELECT * FROM same('SELECT d.label, count(*) AS n, sum(f.v) AS s FROM tt f JOIN th_dim d ON d.g = f.g WHERE f.ctid >= ''(10,4)'' AND f.ctid < ''(300,2)'' AND d.g <> 3 GROUP BY d.label');

-- an invalid constant is an error as in any plan
SELECT count(*) FROM th WHERE ctid >= '(1,2,3)';

DROP FUNCTION units_of(text);
DROP FUNCTION all_ranges(text);
DROP FUNCTION th_q(int, int);
DROP FUNCTION same(text);
DROP FUNCTION plan_of(text);
DROP TABLE tp, tr_range, tt, th, th_before, th_dim, tg, tu;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
