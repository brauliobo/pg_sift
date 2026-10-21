-- The arms of a UNION ALL that read the same rows for the same aggregates run in one pass over the index: arms with a lookup join, with keys
-- of their own, with leaves only some of them read, and arms whose rows the index cannot store (read again from the heap once, not per arm).
CREATE EXTENSION pg_sift;
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
CREATE FUNCTION arms_shared(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; n int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ 'Arms In One Pass: ' THEN n := greatest(n, substr(l, strpos(l, ': ') + 2)::int); END IF;
	END LOOP;
	RETURN n;
END $$;

CREATE FUNCTION same(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE d bigint; n bigint;
BEGIN
	SET sift.enable_agg = on;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	SELECT count(*) INTO d FROM ((TABLE r_on EXCEPT ALL TABLE r_off) UNION ALL (TABLE r_off EXCEPT ALL TABLE r_on)) x;
	SELECT count(*) INTO n FROM r_on;
	DROP TABLE r_on, r_off;
	RETURN d = 0 AND n > 0;
END $$;

-- rows the scan read again from the heap, as the first node of the plan that shows it reports them
CREATE FUNCTION reread(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ 'Rows Read Again From The Heap: ' THEN RETURN substr(l, strpos(l, ': ') + 2)::bigint; END IF;
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

-- not in one pass: another WHERE, other aggregates
SELECT arms_shared($$SELECT kind AS k, count(*) AS n FROM ev WHERE qty > 2 GROUP BY kind UNION ALL SELECT zone_id::text, count(*) FROM ev GROUP BY zone_id$$) AS other_filter_shared;
SELECT arms_shared($$SELECT kind AS k, count(*) AS n FROM ev GROUP BY kind UNION ALL SELECT zone_id::text, sum(qty) FROM ev GROUP BY zone_id$$) AS other_aggregate_shared;

-- the shared database of a plain installcheck goes on with the next test
SET client_min_messages = warning;
DROP SCHEMA public CASCADE;
CREATE SCHEMA public;
GRANT USAGE ON SCHEMA public TO PUBLIC;
RESET client_min_messages;
DROP EXTENSION pg_sift;
