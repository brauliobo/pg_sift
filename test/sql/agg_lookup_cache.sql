-- The rows a Custom Scan (SiftAgg) loads from a joined table are kept by the backend for the next query ("Cached Lookup Tables" of EXPLAIN ANALYZE) while
-- the table is what it was: every page visible to all transactions (vacuumed) and not written to since. Every statement is run with the cache, with
-- sift.agg_lookup_cache_kb = 0 and with sift.enable_agg = off: the rows must be the same, whatever happened to the table in between.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;

CREATE TABLE ev (id int PRIMARY KEY, dim_a_id int, foreign_id int, day date, kind text, amount numeric(10, 2)) WITH (autovacuum_enabled = false);
INSERT INTO ev
SELECT i, CASE WHEN i % 997 = 0 THEN NULL ELSE 1 + i % 3000 END, 1 + i % 40, date '2024-01-01' + i % 700, (ARRAY['a', 'b', 'c', 'd'])[1 + i % 4], (i % 1000) / 10.0
FROM generate_series(1, 120000) i;
CREATE INDEX ev_col ON ev USING sift_col (dim_a_id, foreign_id, day, kind, amount);
VACUUM (FREEZE, ANALYZE) ev;
CREATE TABLE ents (id int, country int, tier int, label text) WITH (autovacuum_enabled = false);
INSERT INTO ents SELECT i, i % 30, i % 4, 'e' || i FROM generate_series(1, 3000) i;
VACUUM (FREEZE, ANALYZE) ents;

-- the tables the node of an EXPLAIN ANALYZE took from an earlier query
CREATE FUNCTION cached(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; nodes int := 0; kept int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN nodes := nodes + 1; END IF;
		IF l LIKE '%Cached Lookup Tables:%' THEN kept := kept + trim(split_part(l, ':', 2))::int; END IF;
	END LOOP;
	RETURN format('%s SiftAgg nodes, %s cached tables', nodes, kept);
END $$;

CREATE FUNCTION same(q text) RETURNS TABLE (result_rows bigint, vs_uncached bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS %s', q);
	SET LOCAL sift.agg_lookup_cache_kb = 0;
	EXECUTE format('CREATE TEMP TABLE r_gen ON COMMIT DROP AS %s', q);
	RESET sift.agg_lookup_cache_kb;
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', q);
	RESET sift.enable_agg;
	result_rows := (SELECT count(*) FROM r_on);
	vs_uncached := (SELECT count(*) FROM ((TABLE r_on EXCEPT TABLE r_gen) UNION ALL (TABLE r_gen EXCEPT TABLE r_on)) d);
	vs_normal_plan := (SELECT count(*) FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d);
	DROP TABLE r_on, r_gen, r_off;
	RETURN NEXT;
END $$;

\set tier_label_lt10 'SELECT e.tier, e.label, count(*) AS n, sum(amount)::bigint AS total FROM ev JOIN ents e ON e.id = ev.dim_a_id WHERE e.country < 10 GROUP BY 1, 2'
\set tier_lt5 'SELECT e.tier, count(*) AS n FROM ev JOIN ents e ON e.id = ev.dim_a_id WHERE e.country < 5 GROUP BY 1'

-- the first query loads the table, the next ones use it; another condition is another table
SELECT cached(:'tier_label_lt10');
SELECT cached(:'tier_label_lt10');
SELECT * FROM same(:'tier_label_lt10');
SELECT cached(:'tier_lt5');
SELECT cached(:'tier_lt5');
SELECT cached(:'tier_label_lt10');

-- a write to the table: the rows are loaded again, from pages that are no longer all visible, and not kept until it is vacuumed
INSERT INTO ents VALUES (7, 7, 3, 'new');
SELECT cached(:'tier_label_lt10');
SELECT * FROM same(:'tier_label_lt10');
SELECT cached(:'tier_label_lt10');
VACUUM ents;
SELECT cached(:'tier_label_lt10');
SELECT cached(:'tier_label_lt10');
SELECT * FROM same(:'tier_label_lt10');

-- an update and a delete, each vacuumed
UPDATE ents SET tier = 9, label = 'moved' WHERE id = 8;
VACUUM ents;
SELECT cached(:'tier_label_lt10');
SELECT * FROM same(:'tier_label_lt10');
SELECT cached(:'tier_label_lt10');
DELETE FROM ents WHERE id < 100;
VACUUM ents;
SELECT cached(:'tier_label_lt10');
SELECT * FROM same(:'tier_label_lt10');
SELECT cached(:'tier_label_lt10');

-- a write that is rolled back moves the pages too, and the rows are what they were
BEGIN;
UPDATE ents SET label = 'rolled back' WHERE id = 500;
SELECT cached(:'tier_label_lt10');
SELECT * FROM same(:'tier_label_lt10');
ROLLBACK;
SELECT * FROM same(:'tier_label_lt10');
VACUUM ents;
SELECT cached(:'tier_label_lt10');
SELECT cached(:'tier_label_lt10');

-- a transaction that sees its own uncommitted rows never takes the rows of another snapshot
BEGIN;
INSERT INTO ents VALUES (11, 5, 2, 'mine');
SELECT * FROM same(:'tier_label_lt10');
ROLLBACK;
SELECT * FROM same(:'tier_label_lt10');

-- rewritten table: another file, whose pages are visible to all once it is vacuumed
VACUUM FULL ents;
SELECT cached(:'tier_label_lt10');
VACUUM ents;
SELECT cached(:'tier_label_lt10');
SELECT cached(:'tier_label_lt10');
SELECT * FROM same(:'tier_label_lt10');

-- off: nothing is kept
SET sift.agg_lookup_cache_kb = 0;
SELECT cached(:'tier_label_lt10');
SELECT cached(:'tier_label_lt10');
RESET sift.agg_lookup_cache_kb;

-- a preserved side is written to by the scan (the rows no tuple matched): it is never kept, and every run sees its own
\set full_join_label 'SELECT coalesce(e.label, ''none'') AS nm, count(*) AS n, sum(ev.amount) AS total FROM ev FULL JOIN ents e ON e.id = ev.dim_a_id GROUP BY 1'
SET enable_seqscan = off; SET enable_indexscan = off; SET enable_bitmapscan = off; SET enable_indexonlyscan = off; SET enable_hashjoin = off; SET enable_mergejoin = off;
SELECT cached(:'full_join_label');
SELECT cached(:'full_join_label');
SELECT * FROM same(:'full_join_label');
RESET enable_seqscan; RESET enable_indexscan; RESET enable_bitmapscan; RESET enable_indexonlyscan; RESET enable_hashjoin; RESET enable_mergejoin;
-- a table that does not fit the budget is loaded for its query alone
SET sift.agg_lookup_cache_kb = 8;
\set tier_label_lt11 'SELECT e.tier, e.label, count(*) AS n FROM ev JOIN ents e ON e.id = ev.dim_a_id WHERE e.country < 11 GROUP BY 1, 2'
SELECT cached(:'tier_label_lt11');
SELECT cached(:'tier_label_lt11');
SELECT * FROM same(:'tier_label_lt11');
RESET sift.agg_lookup_cache_kb;
-- ... and is kept once the budget is larger (the refusal is of that budget): the memory of the rows decides, not the pages of the table
SELECT cached(:'tier_label_lt11');
SELECT cached(:'tier_label_lt11');
SELECT * FROM same(:'tier_label_lt11');
-- a condition that is not constant (a stable function) is another query's table every time: not kept
SELECT cached('SELECT e.tier, count(*) AS n FROM ev JOIN ents e ON e.id = ev.dim_a_id WHERE e.country < extract(day from current_date)::int - extract(day from current_date)::int + 10 GROUP BY 1');
SELECT cached('SELECT e.tier, count(*) AS n FROM ev JOIN ents e ON e.id = ev.dim_a_id WHERE e.country < extract(day from current_date)::int - extract(day from current_date)::int + 10 GROUP BY 1');

DROP FUNCTION same(text);
DROP FUNCTION cached(text);
DROP TABLE ev, ents;
DROP EXTENSION pg_sift;
