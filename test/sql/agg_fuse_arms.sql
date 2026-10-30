-- The arms of a UNION ALL that read the same rows for the same aggregates run in one pass over the index: arms with a lookup join, with keys
-- of their own, with leaves only some of them read, and arms whose rows the index cannot store (read again from the heap once, not per arm).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE TABLE zones (id int PRIMARY KEY, name text, tier int);
INSERT INTO zones SELECT i, 'z' || i % 7, i % 3 FROM generate_series(0, 49) i;
CREATE TABLE ev (id int PRIMARY KEY, zone_id int, day date, kind text, qty int, price float8, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO ev
SELECT i, i % 50, '2026-03-01'::date + i % 40, (ARRAY['a', 'b', 'c'])[1 + i % 3], i % 9, (i % 13) + 0.5,
       jsonb_build_object('lines', (SELECT jsonb_agg(jsonb_build_object('k', (ARRAY['x', 'y', 'z'])[1 + (i + j) % 3], 'p', 'p' || (i * j) % 4,
                                                                        'v', CASE WHEN i % 10 = 0 AND j = 1 THEN to_jsonb(((i + j) % 8)::text) ELSE to_jsonb((i + j) % 8 + 0.5) END))
                                    FROM generate_series(1, 1 + i % 4) j))
FROM generate_series(1, 6000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, zone_id, day, kind, qty, price, doc) WITH (paths = 'doc.lines[]{k text, p text, v float8}');
VACUUM (FREEZE, ANALYZE) ev;
ANALYZE zones;

-- the most arms one scan served in the plan; the rows must be those of the normal plan
CREATE FUNCTION arms_shared(q text) RETURNS int LANGUAGE sql AS $$ SELECT sift_helpers.agg_arms_in_one_pass(q) $$;

CREATE FUNCTION same(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.agg_equals_normal_all(q) $$;

-- rows the scan read again from the heap, as the first node of the plan that shows it reports them
CREATE FUNCTION reread(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ '(\d+) re-read from the heap' THEN RETURN substring(l from '(\d+) re-read from the heap')::bigint; END IF;
	END LOOP;
	RETURN -1;
END $$;

-- three arms with a lookup join: the table is read once, the join key and the zone are shared
\set join_arms 'SELECT z.name AS a, NULL::text AS b, count(*) AS n, sum(e.price) AS s FROM ev e JOIN zones z ON z.id = e.zone_id WHERE e.day >= ''2026-03-10'' GROUP BY z.name UNION ALL SELECT z.name, e.kind, count(*), sum(e.price) FROM ev e JOIN zones z ON z.id = e.zone_id WHERE e.day >= ''2026-03-10'' GROUP BY z.name, e.kind UNION ALL SELECT z.name, e.day::text, count(*), sum(e.price) FROM ev e JOIN zones z ON z.id = e.zone_id WHERE e.day >= ''2026-03-10'' GROUP BY z.name, e.day'
SELECT arms_shared(:'join_arms') AS join_arms_shared, same(:'join_arms') AS join_arms_same;

-- arms that group by an expression of the joined table and by columns of the rows
SELECT arms_shared($$SELECT z.tier AS a, count(*) AS n, avg(e.price) AS m FROM ev e JOIN zones z ON z.id = e.zone_id GROUP BY z.tier
	UNION ALL SELECT z.tier * 10 + e.qty, count(*), avg(e.price) FROM ev e JOIN zones z ON z.id = e.zone_id GROUP BY z.tier, e.qty$$) AS tier_arms_shared;
SELECT same($$SELECT z.tier AS a, count(*) AS n, avg(e.price) AS m FROM ev e JOIN zones z ON z.id = e.zone_id GROUP BY z.tier
	UNION ALL SELECT z.tier * 10 + e.qty, count(*), avg(e.price) FROM ev e JOIN zones z ON z.id = e.zone_id GROUP BY z.tier, e.qty$$) AS tier_arms_same;

-- more keys among the arms than one pass once allowed, the same key of two arms counted once
SELECT arms_shared($$SELECT zone_id AS a, NULL::int AS b, NULL::int AS c, NULL::text AS d, NULL::date AS e, count(*) AS n FROM ev GROUP BY zone_id
	UNION ALL SELECT zone_id, qty, NULL, NULL, NULL, count(*) FROM ev GROUP BY zone_id, qty
	UNION ALL SELECT zone_id, qty, id % 7, NULL, NULL, count(*) FROM ev GROUP BY zone_id, qty, id % 7
	UNION ALL SELECT zone_id, qty, id % 7, kind, NULL, count(*) FROM ev GROUP BY zone_id, qty, id % 7, kind
	UNION ALL SELECT zone_id, qty, id % 7, kind, day, count(*) FROM ev GROUP BY zone_id, qty, id % 7, kind, day$$) AS wide_arms_shared;
SELECT same($$SELECT zone_id AS a, NULL::int AS b, NULL::int AS c, NULL::text AS d, NULL::date AS e, count(*) AS n FROM ev GROUP BY zone_id
	UNION ALL SELECT zone_id, qty, NULL, NULL, NULL, count(*) FROM ev GROUP BY zone_id, qty
	UNION ALL SELECT zone_id, qty, id % 7, NULL, NULL, count(*) FROM ev GROUP BY zone_id, qty, id % 7
	UNION ALL SELECT zone_id, qty, id % 7, kind, NULL, count(*) FROM ev GROUP BY zone_id, qty, id % 7, kind
	UNION ALL SELECT zone_id, qty, id % 7, kind, day, count(*) FROM ev GROUP BY zone_id, qty, id % 7, kind, day$$) AS wide_arms_same;

-- arms that read leaves of their own
SELECT arms_shared($$SELECT kind AS k, count(*) AS n, sum(qty) AS s FROM ev GROUP BY kind UNION ALL SELECT zone_id::text, count(*), sum(qty) FROM ev GROUP BY zone_id$$) AS other_leaves_shared;
SELECT same($$SELECT kind AS k, count(*) AS n, sum(qty) AS s FROM ev GROUP BY kind UNION ALL SELECT zone_id::text, count(*), sum(qty) FROM ev GROUP BY zone_id$$) AS other_leaves_same;

-- the lines of a document, some with a score stored as text (rows the index reads again from the heap): once for the arms together
\set line_arms 'SELECT l.k AS a, NULL::text AS b, count(*) AS n, avg(l.v) AS m FROM ev e CROSS JOIN LATERAL jsonb_to_recordset(e.doc -> ''lines'') AS l(k text, p text, v float8) GROUP BY l.k UNION ALL SELECT l.k, l.p, count(*), avg(l.v) FROM ev e CROSS JOIN LATERAL jsonb_to_recordset(e.doc -> ''lines'') AS l(k text, p text, v float8) GROUP BY l.k, l.p UNION ALL SELECT NULL, l.p, count(*), avg(l.v) FROM ev e CROSS JOIN LATERAL jsonb_to_recordset(e.doc -> ''lines'') AS l(k text, p text, v float8) GROUP BY l.p'
\set line_arm 'SELECT l.k AS a, NULL::text AS b, count(*) AS n, avg(l.v) AS m FROM ev e CROSS JOIN LATERAL jsonb_to_recordset(e.doc -> ''lines'') AS l(k text, p text, v float8) GROUP BY l.k'
SELECT arms_shared(:'line_arms') AS line_arms_shared, same(:'line_arms') AS line_arms_same;
SELECT reread(:'line_arm') > 0 AS arm_rereads, reread(:'line_arms') = reread(:'line_arm') AS rereads_once;

-- an arm without keys (the total) is a set of the pass too, and has its row when no row reached it; so has a total over a lookup join
\set total_arms 'SELECT kind AS a, NULL::int AS b, count(*) AS n, sum(price) AS s FROM ev GROUP BY kind UNION ALL SELECT NULL, qty, count(*), sum(price) FROM ev GROUP BY qty UNION ALL SELECT NULL, NULL, count(*), sum(price) FROM ev'
\set empty_total_arms 'SELECT kind AS a, count(*) AS n, sum(price) AS s FROM ev WHERE qty > 100 GROUP BY kind UNION ALL SELECT NULL, count(*), sum(price) FROM ev WHERE qty > 100'
\set join_total_arms 'SELECT z.name AS a, count(*) AS n, sum(e.price) AS s FROM ev e JOIN zones z ON z.id = e.zone_id GROUP BY z.name UNION ALL SELECT NULL, count(*), sum(e.price) FROM ev e JOIN zones z ON z.id = e.zone_id'
SELECT arms_shared(:'total_arms') AS total_arms_shared, same(:'total_arms') AS total_arms_same;
SELECT arms_shared(:'empty_total_arms') AS empty_total_shared, same(:'empty_total_arms') AS empty_total_same;
SELECT arms_shared(:'join_total_arms') AS join_total_shared, same(:'join_total_arms') AS join_total_same;

-- not in one pass: another WHERE, other aggregates
SELECT arms_shared($$SELECT kind AS k, count(*) AS n FROM ev WHERE qty > 2 GROUP BY kind UNION ALL SELECT zone_id::text, count(*) FROM ev GROUP BY zone_id$$) AS other_filter_shared;
SELECT arms_shared($$SELECT kind AS k, count(*) AS n FROM ev GROUP BY kind UNION ALL SELECT zone_id::text, sum(qty) FROM ev GROUP BY zone_id$$) AS other_aggregate_shared;

-- the rest of an ON clause that only one arm has: an inner join drops the rows it refuses in the arm's set only; a left join keeps them (another set of rows)
\set inner_plain 'SELECT z.name AS k, count(*) AS n, sum(e.price) AS s FROM ev e JOIN zones z ON z.id = e.zone_id GROUP BY z.name'
\set inner_rest 'SELECT e.kind::text, count(*), sum(e.price) FROM ev e JOIN zones z ON z.id = e.zone_id AND z.tier > 0 GROUP BY e.kind'
\set left_plain 'SELECT z.name AS k, count(*) AS n, sum(e.price) AS s FROM ev e LEFT JOIN zones z ON z.id = e.zone_id GROUP BY z.name'
\set left_rest 'SELECT e.kind::text, count(*), sum(e.price) FROM ev e LEFT JOIN zones z ON z.id = e.zone_id AND z.tier > 0 GROUP BY e.kind'
SELECT arms_shared(:'inner_rest' || ' UNION ALL ' || :'inner_plain') AS inner_rest_shared, same(:'inner_rest' || ' UNION ALL ' || :'inner_plain') AS inner_rest_same;
SELECT arms_shared(:'left_rest' || ' UNION ALL ' || :'left_plain') AS left_rest_shared, same(:'left_rest' || ' UNION ALL ' || :'left_plain') AS left_rest_same;

-- arms that look the mapped names of each line up with a correlated LIMIT 1 sub-select: the arms read other columns of it (one the mapped kind, one
-- also the mapped part), the lines without a mapping are no row of the first two arms (an inner join) and a row of the third (a left join): one pass keeps them for that arm only
-- (the scan of the table is made dearer than the index's, and the lookups are not cached: the plan with the sub-select as a child of the scan is the one PostgreSQL costs cheapest)
SET enable_seqscan = off;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SET enable_indexonlyscan = off;
SET enable_memoize = off;
CREATE TABLE kmap (k text, p text, mk text, mp text);
INSERT INTO kmap SELECT k, 'p' || p, 'm' || (p % 2) || k, CASE WHEN k = 'y' THEN NULL ELSE 'q' || p END FROM unnest(ARRAY['x', 'y']) k, generate_series(0, 2) p;
CREATE INDEX kmap_i ON kmap (k, p);
ANALYZE kmap;
\set flag_a 'WITH m AS NOT MATERIALIZED (SELECT e.id, l.v, mp.mk, mp.mp FROM ev e CROSS JOIN LATERAL jsonb_to_recordset(e.doc -> ''lines'') AS l(k text, p text, v float8) LEFT JOIN LATERAL (SELECT mk, mp FROM kmap x WHERE x.k = l.k AND x.p = l.p LIMIT 1) mp ON true WHERE e.day >= ''2026-03-05'' AND l.v IS NOT NULL)'
\set arm_names 'SELECT id, mk, mp, avg(v) AS m, count(*) AS n FROM m WHERE mk IS NOT NULL GROUP BY id, mk, mp'
\set arm_kind 'SELECT id, mk, NULL::text, avg(v), count(*) FROM m WHERE mk IS NOT NULL GROUP BY id, mk'
\set arm_all 'SELECT id, NULL::text, NULL::text, avg(v), count(*) FROM m GROUP BY id'
SELECT arms_shared(:'flag_a' || :'arm_names' || ' UNION ALL ' || :'arm_kind') AS mapped_arms_shared, same(:'flag_a' || :'arm_names' || ' UNION ALL ' || :'arm_kind') AS mapped_arms_same;
SELECT arms_shared(:'flag_a' || :'arm_names' || ' UNION ALL ' || :'arm_kind' || ' UNION ALL ' || :'arm_all') AS mapped_three_shared,
       same(:'flag_a' || :'arm_names' || ' UNION ALL ' || :'arm_kind' || ' UNION ALL ' || :'arm_all') AS mapped_three_same;
SELECT reread(:'flag_a' || :'arm_names') > 0 AS mapped_arm_rereads,
       reread(:'flag_a' || :'arm_names' || ' UNION ALL ' || :'arm_kind') = reread(:'flag_a' || :'arm_names') AS mapped_rereads_once,
       reread(:'flag_a' || :'arm_names' || ' UNION ALL ' || :'arm_kind' || ' UNION ALL ' || :'arm_all') = reread(:'flag_a' || :'arm_names') AS mapped_three_rereads_once;
RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_bitmapscan;
RESET enable_indexonlyscan;
RESET enable_memoize;

-- arms that join a common table expression of periods (a VALUES list, read by every arm): the arms are the same pass when they join the same CTE with the
-- same conditions, whatever columns of it each reads; another list or another condition keeps its own pass. The list is loaded once for the pass.
\set periods 'WITH periods AS (SELECT * FROM (VALUES (''cur'', daterange(''2026-03-05'', ''2026-03-20'', ''[]'')), (''prev'', daterange(''2026-03-25'', ''2026-04-05'', ''[]''))) AS v(label, span))'
\set per_both 'SELECT p.label, e.kind, count(*) AS n, sum(e.price) AS s FROM ev e JOIN periods p ON e.day <@ p.span GROUP BY p.label, e.kind'
\set per_label 'SELECT p.label, NULL, count(*), sum(e.price) FROM ev e JOIN periods p ON e.day <@ p.span GROUP BY p.label'
\set per_kind 'SELECT NULL, e.kind, count(*), sum(e.price) FROM ev e JOIN periods p ON e.day <@ p.span GROUP BY e.kind'
SELECT arms_shared(:'periods' || :'per_both' || ' UNION ALL ' || :'per_label') AS periods_arms_shared, same(:'periods' || :'per_both' || ' UNION ALL ' || :'per_label') AS periods_arms_same;
SELECT arms_shared(:'periods' || :'per_both' || ' UNION ALL ' || :'per_label' || ' UNION ALL ' || :'per_kind') AS periods_three_shared,
       same(:'periods' || :'per_both' || ' UNION ALL ' || :'per_label' || ' UNION ALL ' || :'per_kind') AS periods_three_same;
-- a condition on the CTE in one arm
SELECT arms_shared(:'periods' || 'SELECT p.label, e.kind, count(*) FROM ev e JOIN periods p ON e.day <@ p.span WHERE p.label = ''cur'' GROUP BY p.label, e.kind UNION ALL SELECT p.label, NULL, count(*) FROM ev e JOIN periods p ON e.day <@ p.span GROUP BY p.label') AS periods_condition_shared,
       same(:'periods' || 'SELECT p.label, e.kind, count(*) FROM ev e JOIN periods p ON e.day <@ p.span WHERE p.label = ''cur'' GROUP BY p.label, e.kind UNION ALL SELECT p.label, NULL, count(*) FROM ev e JOIN periods p ON e.day <@ p.span GROUP BY p.label') AS periods_condition_same;
-- two lists that differ
\set two_lists 'WITH a AS (SELECT * FROM (VALUES (''cur'', daterange(''2026-03-05'', ''2026-03-20'', ''[]''))) AS v(label, span)), b AS (SELECT * FROM (VALUES (''cur'', daterange(''2026-03-05'', ''2026-03-21'', ''[]''))) AS v(label, span)) SELECT p.label, e.kind, count(*) FROM ev e JOIN a p ON e.day <@ p.span GROUP BY p.label, e.kind UNION ALL SELECT p.label, NULL, count(*) FROM ev e JOIN b p ON e.day <@ p.span GROUP BY p.label'
SELECT arms_shared(:'two_lists') AS two_lists_shared, same(:'two_lists') AS two_lists_same;
-- the lines of each event (a list the index cannot always store): the rows read again from the heap are read once for the three arms
\set per_lines 'FROM ev e CROSS JOIN LATERAL jsonb_to_recordset(e.doc -> ''lines'') AS l(k text, p text, v float8) JOIN periods p ON e.day <@ p.span WHERE l.v IS NOT NULL'
\set lines_both 'SELECT p.label, l.k, avg(l.v), count(*) ' :per_lines ' GROUP BY p.label, l.k'
\set lines_label 'SELECT p.label, NULL, avg(l.v), count(*) ' :per_lines ' GROUP BY p.label'
\set lines_kind 'SELECT NULL, l.k, avg(l.v), count(*) ' :per_lines ' GROUP BY l.k'
SELECT arms_shared(:'periods' || :'lines_both' || ' UNION ALL ' || :'lines_label' || ' UNION ALL ' || :'lines_kind') AS periods_lines_shared,
       same(:'periods' || :'lines_both' || ' UNION ALL ' || :'lines_label' || ' UNION ALL ' || :'lines_kind') AS periods_lines_same,
       reread(:'periods' || :'lines_both') > 0 AS periods_lines_reread,
       reread(:'periods' || :'lines_both' || ' UNION ALL ' || :'lines_label' || ' UNION ALL ' || :'lines_kind') = reread(:'periods' || :'lines_both') AS periods_lines_once;

-- the shared database of a plain installcheck goes on with the next test
SET client_min_messages = warning;
DROP SCHEMA public CASCADE;
CREATE SCHEMA public;
GRANT USAGE ON SCHEMA public TO PUBLIC;
RESET client_min_messages;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
