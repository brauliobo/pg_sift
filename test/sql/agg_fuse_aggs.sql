-- UNION ALL arms that join the same lookup and read the same rows but ask for other aggregates run in one pass over the index: the pass has the
-- union of the aggregates and each set updates its own. Arms whose aggregates are numbered by their own list (a percentile) keep their scans.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE TABLE dims (id int PRIMARY KEY, label text, tier int);
INSERT INTO dims SELECT i, CASE WHEN i % 9 = 0 THEN NULL ELSE 'l' || i % 6 END, CASE WHEN i % 7 = 0 THEN NULL ELSE i % 3 END FROM generate_series(0, 399) i;
CREATE TABLE ev (id int PRIMARY KEY, grp int, ref int, kind text, day date, qty int, price float8, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO ev
SELECT i, CASE WHEN i % 11 = 0 THEN NULL ELSE i % 7 END, CASE WHEN i % 19 = 0 THEN NULL ELSE i % 420 END, CASE WHEN i % 13 = 0 THEN NULL ELSE (ARRAY['a', 'b', 'c'])[1 + i % 3] END,
       '2026-03-01'::date + i % 40, CASE WHEN i % 17 = 0 THEN NULL ELSE i % 9 END, (i % 13) + 0.5,
       jsonb_build_object('lines', (SELECT jsonb_agg(jsonb_build_object('k', (ARRAY['x', 'y', 'z'])[1 + (i + j) % 3], 'p', 'p' || (i * j) % 4,
                                                                        'v', CASE WHEN i % 10 = 0 AND j = 1 THEN to_jsonb(((i + j) % 8)::text) ELSE to_jsonb((i + j) % 8 + 0.5) END))
                                    FROM generate_series(1, 1 + i % 4) j))
FROM generate_series(1, 6000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, grp, ref, kind, day, qty, price, doc) WITH (paths = 'doc.lines[]{k text, p text, v float8}');
VACUUM (FREEZE, ANALYZE) ev;
ANALYZE dims;

-- the most arms one scan served in the plan
CREATE FUNCTION arms_shared(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; n int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ 'Arms In One Pass: ' THEN n := greatest(n, substr(l, strpos(l, ': ') + 2)::int); END IF;
	END LOOP;
	RETURN n;
END $$;

-- the rows are those of the normal plan, and those of the arms each in a scan of its own
CREATE FUNCTION same(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE d bigint; n bigint;
BEGIN
	SET sift.enable_agg = on;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SET sift.agg_fuse = off;
	EXECUTE format('CREATE TEMP TABLE r_alone AS %s', q);
	RESET sift.agg_fuse;
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	SELECT count(*) INTO d FROM ((TABLE r_on EXCEPT ALL TABLE r_off) UNION ALL (TABLE r_off EXCEPT ALL TABLE r_on)
								 UNION ALL (TABLE r_on EXCEPT ALL TABLE r_alone) UNION ALL (TABLE r_alone EXCEPT ALL TABLE r_on)) x;
	SELECT count(*) INTO n FROM r_on;
	DROP TABLE r_on, r_alone, r_off;
	RETURN d = 0 AND n > 0;
END $$;

\set from 'FROM ev e JOIN dims d ON d.id = e.ref'
\set lines_from 'FROM ev e JOIN dims d ON d.id = e.ref CROSS JOIN LATERAL jsonb_to_recordset(e.doc -> ''lines'') AS l(k text, p text, v float8)'

-- other counts, sums, averages, minima and maxima per arm, a DISTINCT count and FILTERs in one, NULL keys
\set simple 'SELECT d.label AS a, NULL::text AS b, count(*) AS n, sum(e.price) AS s, avg(e.price) AS m ' :from ' GROUP BY d.label UNION ALL SELECT d.label, e.kind, count(e.qty), min(e.price), max(e.price) ' :from ' GROUP BY d.label, e.kind UNION ALL SELECT NULL, e.day::text, count(DISTINCT e.grp), sum(e.price) FILTER (WHERE e.qty > 3), avg(e.price) FILTER (WHERE e.kind = ''a'') ' :from ' GROUP BY e.day'
SELECT arms_shared(:'simple') AS simple_shared, same(:'simple') AS simple_same;

-- aggregates of the lookup's columns (the ids of their values), beside those of the rows
\set flag_a 'SELECT e.kind AS a, count(*) AS n, sum(e.qty) AS s ' :from ' GROUP BY e.kind UNION ALL SELECT d.label, count(d.label), sum(d.tier) ' :from ' GROUP BY d.label UNION ALL SELECT e.kind, count(DISTINCT d.label), max(d.tier) ' :from ' GROUP BY e.kind'
SELECT arms_shared(:'flag_a') AS mapped_shared, same(:'flag_a') AS mapped_same;

-- the same aggregate in two arms is one of the pass, and one arm asks for it twice
\set twice 'SELECT e.kind AS a, count(*) AS n, count(*) AS m ' :from ' GROUP BY e.kind UNION ALL SELECT d.label, count(*), sum(e.qty) ' :from ' GROUP BY d.label UNION ALL SELECT NULL, count(*), count(e.qty) ' :from
SELECT arms_shared(:'twice') AS twice_shared, same(:'twice') AS twice_same;

-- other aggregates and a WHERE of their own
\set where_aggs 'SELECT e.kind AS a, count(*) AS n, sum(e.price) AS s ' :from ' WHERE e.day >= ''2026-03-10'' AND e.qty > 3 GROUP BY e.kind UNION ALL SELECT d.label, count(e.qty), max(e.price) ' :from ' WHERE e.day >= ''2026-03-10'' GROUP BY d.label'
SELECT arms_shared(:'where_aggs') AS where_aggs_shared, same(:'where_aggs') AS where_aggs_same;

-- an arm that no row reaches has no group
\set none 'SELECT e.kind AS a, count(*) AS n, sum(e.qty) AS s ' :from ' WHERE e.qty > 100 GROUP BY e.kind UNION ALL SELECT d.label, count(e.qty), min(e.qty) ' :from ' GROUP BY d.label'
SELECT arms_shared(:'none') AS none_shared, same(:'none') AS none_same;

-- the lines of a document (some with a score stored as text, which the index reads again from the heap): other aggregates per arm
\set lines 'SELECT l.k AS a, NULL::text AS b, count(*) AS n, avg(l.v) AS m ' :lines_from ' GROUP BY l.k UNION ALL SELECT l.k, l.p, count(l.v), max(l.v) ' :lines_from ' GROUP BY l.k, l.p UNION ALL SELECT NULL, l.p, count(*), min(l.v) ' :lines_from ' GROUP BY l.p'
SELECT arms_shared(:'lines') AS lines_shared, same(:'lines') AS lines_same;

-- a percentile or GROUPING number their own aggregates and keys: arms with other aggregates and one of them keep their scans
\set pct 'SELECT e.kind AS a, count(*) AS n, percentile_cont(0.5) WITHIN GROUP (ORDER BY e.price) AS m ' :from ' GROUP BY e.kind UNION ALL SELECT d.label, count(e.qty), percentile_cont(0.5) WITHIN GROUP (ORDER BY e.price) ' :from ' GROUP BY d.label'
SELECT arms_shared(:'pct') AS pct_shared, same(:'pct') AS pct_same;
\set pct_other 'SELECT e.kind AS a, count(*) AS n, percentile_cont(0.5) WITHIN GROUP (ORDER BY e.price) AS m ' :from ' GROUP BY e.kind UNION ALL SELECT d.label, count(e.qty), max(e.price) ' :from ' GROUP BY d.label'
SELECT arms_shared(:'pct_other') AS pct_other_shared, same(:'pct_other') AS pct_other_same;

-- arms that look nothing up keep their scans, as the arms with another WHERE do
\set no_join 'SELECT kind AS a, count(*) AS n, sum(qty) AS s FROM ev GROUP BY kind UNION ALL SELECT grp::text, count(qty), min(qty) FROM ev GROUP BY grp'
SELECT arms_shared(:'no_join') AS no_join_shared, same(:'no_join') AS no_join_same;

-- rows changed after the index was built are read from the heap
UPDATE ev SET qty = 1, kind = 'b' WHERE id % 97 = 0;
UPDATE ev SET qty = NULL, day = '2026-04-20' WHERE id % 89 = 0;
DELETE FROM ev WHERE id % 101 = 0;
SELECT arms_shared(:'simple') AS simple_shared, same(:'simple') AS simple_same;
SELECT arms_shared(:'flag_a') AS mapped_shared, same(:'flag_a') AS mapped_same;
SELECT arms_shared(:'lines') AS lines_shared, same(:'lines') AS lines_same;
SELECT arms_shared(:'where_aggs') AS where_aggs_shared, same(:'where_aggs') AS where_aggs_same;

-- the workers of the scan: the sets' rows are the same
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SET sift.agg_preagg = off;
SELECT same(:'simple') AS simple_par_same, same(:'flag_a') AS mapped_par_same, same(:'lines') AS lines_par_same, same(:'where_aggs') AS where_aggs_par_same;

DROP FUNCTION arms_shared(text), same(text);
DROP TABLE ev, dims;
DROP EXTENSION pg_sift;
