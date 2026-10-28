-- The rows a Custom Scan (SiftAgg) loads from a joined table can be kept in shared memory (sift.agg_lookup_shared_kb) for the queries of other backends: this test
-- turns the cache of the backend off (sift.agg_lookup_cache_kb = 0), so that every "Cached Lookup Tables" is a segment of the shared cache, read in place. The rows
-- must be those of the normal plan, whatever happened to the table in between; the isolation spec agg_lookup_shared has the second backend.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;
SET sift.agg_lookup_cache_kb = 0;
SET sift.agg_lookup_shared_kb = 4096;

CREATE TABLE ev (id int PRIMARY KEY, dim_a_id int, foreign_id int, day date, kind text, amount numeric(10, 2)) WITH (autovacuum_enabled = false);
INSERT INTO ev
SELECT i, CASE WHEN i % 997 = 0 THEN NULL ELSE 1 + i % 3000 END, 1 + i % 40, date '2024-01-01' + i % 700, (ARRAY['a', 'b', 'c', 'd'])[1 + i % 4], (i % 1000) / 10.0
FROM generate_series(1, 60000) i;
CREATE INDEX ev_col ON ev USING sift_col (dim_a_id, foreign_id, day, kind, amount);
VACUUM (FREEZE, ANALYZE) ev;
CREATE TABLE ents (id int, country int, tier int, label text, code text) WITH (autovacuum_enabled = false);
INSERT INTO ents SELECT i, i % 30, i % 4, 'e' || i, 'c' || i FROM generate_series(1, 3000) i;
VACUUM (FREEZE, ANALYZE) ents;

CREATE FUNCTION cached(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; nodes int := 0; kept int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN nodes := nodes + 1; END IF;
		IF l LIKE '%Cached Lookup Tables:%' THEN kept := kept + trim(split_part(l, ':', 2))::int; END IF;
	END LOOP;
	RETURN format('%s SiftAgg nodes, %s cached tables', nodes, kept);
END $$;

CREATE FUNCTION same(q text) RETURNS TABLE (result_rows bigint, vs_unshared bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
DECLARE budget text := current_setting('sift.agg_lookup_shared_kb');
BEGIN
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS %s', q);
	SET LOCAL sift.agg_lookup_shared_kb = 0;
	EXECUTE format('CREATE TEMP TABLE r_gen ON COMMIT DROP AS %s', q);
	PERFORM set_config('sift.agg_lookup_shared_kb', budget, true);
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', q);
	RESET sift.enable_agg;
	result_rows := (SELECT count(*) FROM r_on);
	vs_unshared := (SELECT count(*) FROM ((TABLE r_on EXCEPT TABLE r_gen) UNION ALL (TABLE r_gen EXCEPT TABLE r_on)) d);
	vs_normal_plan := (SELECT count(*) FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d);
	DROP TABLE r_on, r_gen, r_off;
	RETURN NEXT;
END $$;

\set tier_label_lt10 'SELECT e.tier, e.label, count(*) AS n, sum(amount)::bigint AS total FROM ev JOIN ents e ON e.id = ev.dim_a_id WHERE e.country < 10 GROUP BY 1, 2'
\set tier_lt5 'SELECT e.tier, count(*) AS n FROM ev JOIN ents e ON e.id = ev.dim_a_id WHERE e.country < 5 GROUP BY 1'
-- a text key
\set text_key_join 'SELECT e.tier, count(*) AS n FROM ev JOIN ents e ON e.code = ''c'' || ev.dim_a_id GROUP BY 1'

-- the first query loads the table and keeps it, the next ones read it; another condition is another table
SELECT cached(:'tier_label_lt10');
SELECT cached(:'tier_label_lt10');
SELECT * FROM same(:'tier_label_lt10');
SELECT cached(:'tier_lt5');
SELECT cached(:'tier_lt5');
SELECT cached(:'tier_label_lt10');
SELECT cached(:'text_key_join');
SELECT cached(:'text_key_join');
SELECT * FROM same(:'text_key_join');

-- a write: the rows are loaded again from pages that are not all visible, and kept again once vacuumed
INSERT INTO ents VALUES (7, 7, 3, 'new', 'cnew');
SELECT cached(:'tier_label_lt10');
SELECT * FROM same(:'tier_label_lt10');
SELECT cached(:'tier_label_lt10');
VACUUM ents;
SELECT cached(:'tier_label_lt10');
SELECT cached(:'tier_label_lt10');
SELECT * FROM same(:'tier_label_lt10');
UPDATE ents SET tier = 9, label = 'moved', code = 'cmoved' WHERE id = 8;
VACUUM ents;
SELECT cached(:'tier_label_lt10');
SELECT * FROM same(:'tier_label_lt10');
SELECT cached(:'text_key_join');
SELECT * FROM same(:'text_key_join');
DELETE FROM ents WHERE id < 100;
VACUUM ents;
SELECT cached(:'tier_label_lt10');
SELECT * FROM same(:'tier_label_lt10');
SELECT cached(:'tier_label_lt10');

-- rolled back, and a transaction that sees its own rows
BEGIN;
UPDATE ents SET label = 'rolled back' WHERE id = 500;
SELECT cached(:'tier_label_lt10');
SELECT * FROM same(:'tier_label_lt10');
ROLLBACK;
SELECT * FROM same(:'tier_label_lt10');
VACUUM ents;
SELECT cached(:'tier_label_lt10');
SELECT cached(:'tier_label_lt10');
BEGIN;
INSERT INTO ents VALUES (11, 5, 2, 'mine', 'cmine');
SELECT * FROM same(:'tier_label_lt10');
ROLLBACK;
SELECT * FROM same(:'tier_label_lt10');

-- a rewritten table is another file
VACUUM FULL ents;
SELECT cached(:'tier_label_lt10');
VACUUM ents;
SELECT cached(:'tier_label_lt10');
SELECT cached(:'tier_label_lt10');
SELECT * FROM same(:'tier_label_lt10');

-- the rows of one role are not read by another
CREATE ROLE lookshm_other;
GRANT SELECT ON ev, ents TO lookshm_other;
SET ROLE lookshm_other;
SELECT cached(:'tier_label_lt10');
SELECT cached(:'tier_label_lt10');
RESET ROLE;
SELECT cached(:'tier_label_lt10');
DROP OWNED BY lookshm_other;
DROP ROLE lookshm_other;

-- the budget is shared memory of all backends: only a superuser changes it
CREATE ROLE lookshm_plain;
SET ROLE lookshm_plain;
SET sift.agg_lookup_shared_kb = 0;
RESET ROLE;
DROP ROLE lookshm_plain;

-- off: nothing is read and nothing is kept
SET sift.agg_lookup_shared_kb = 0;
SELECT cached(:'tier_label_lt10');
SELECT cached(:'tier_label_lt10');
SET sift.agg_lookup_shared_kb = 4096;
-- a table of more than half of the budget is loaded for its query alone, and kept once the budget is larger
\set qa 'SELECT e.tier, e.label, count(*) AS n FROM ev JOIN ents e ON e.id = ev.dim_a_id WHERE e.tier = 0 GROUP BY 1, 2'
\set qb 'SELECT e.tier, e.label, count(*) AS n FROM ev JOIN ents e ON e.id = ev.dim_a_id WHERE e.tier = 1 GROUP BY 1, 2'
\set qc 'SELECT e.tier, e.label, count(*) AS n FROM ev JOIN ents e ON e.id = ev.dim_a_id WHERE e.tier = 2 GROUP BY 1, 2'
SET sift.agg_lookup_shared_kb = 40;
SELECT cached(:'qa');
SELECT cached(:'qa');
SELECT * FROM same(:'qa');
SET sift.agg_lookup_shared_kb = 4096;
SELECT cached(:'qa');
SELECT cached(:'qa');
-- a budget for two of three tables of the same size: the one that was used least goes when another is kept
SET sift.agg_lookup_shared_kb = 120;
SELECT cached(:'qb');
SELECT cached(:'qc');
SELECT cached(:'qb');
SELECT cached(:'qa');
SELECT cached(:'qb');
SELECT cached(:'qc');
SELECT cached(:'qa');
SELECT * FROM same(:'qa');
SELECT * FROM same(:'qb');
-- the cache of the backend first, when it is on: the same rows
SET sift.agg_lookup_shared_kb = 4096;
RESET sift.agg_lookup_cache_kb;
SELECT cached(:'text_key_join');
SELECT cached(:'text_key_join');
SELECT * FROM same(:'text_key_join');

DROP FUNCTION same(text);
DROP FUNCTION cached(text);
DROP TABLE ev, ents;
DROP EXTENSION pg_sift;
