-- The joined table of a lookup join of Custom Scan (SiftAgg) is loaded for the scan: the rows its own conditions (group_id = 3, country = 5) cannot match are
-- not read (a btree on the leading columns) or not kept (the filter runs while loading), sift.agg_lookup_pushdown (docs/SET-FILTERS.md). Every statement is
-- run with the pushdown, without it, and with sift.enable_agg = off (the normal plan): the rows must be the same.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE ev (id int PRIMARY KEY, dim_a_id int, day date, kind text, amount numeric(10, 2)) WITH (autovacuum_enabled = false);
INSERT INTO ev
SELECT i, CASE WHEN i % 997 = 0 THEN NULL ELSE 1 + (i::bigint * 7919) % 30000 END, date '2024-01-01' + i % 700,
       (ARRAY['a', 'b', 'c', 'd'])[1 + i % 4], (i % 1000) / 10.0
FROM generate_series(1, 300000) i;
CREATE INDEX ev_col ON ev USING sift_col (dim_a_id, day, kind, amount);
VACUUM (FREEZE, ANALYZE) ev;

CREATE TABLE dim_b (group_id int, dim_a_id int, deleted_at timestamptz, role int);
INSERT INTO dim_b SELECT g, 1 + (g * 1000 + i * 37) % 30000, CASE WHEN i % 10 = 0 THEN now() END, i % 3
FROM generate_series(1, 40) g, generate_series(1, 500) i;
INSERT INTO dim_b VALUES (3, NULL, NULL, 0), (3, 11, NULL, 0), (3, 11, NULL, 1);
CREATE INDEX members_group ON dim_b (group_id, role, dim_a_id);
CREATE TABLE members_bare (group_id int, dim_a_id int, deleted_at timestamptz);
INSERT INTO members_bare SELECT group_id, dim_a_id, deleted_at FROM dim_b;
CREATE TABLE dims (id int PRIMARY KEY, country int, tier int2, active boolean);
INSERT INTO dims SELECT i, i % 60, (i % 5)::int2, i % 7 <> 0 FROM generate_series(1, 30000) i;
CREATE INDEX dims_country ON dims (country);
ANALYZE dim_b; ANALYZE members_bare; ANALYZE dims;

-- whether the plan is the pushed one, and what the load of the lookup tables read, kept and used
CREATE FUNCTION plan_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN r := 'SiftAgg'; END IF;
		IF l LIKE '%Lookup Rows Read:%' OR l LIKE '%Lookup Rows Loaded:%' OR l LIKE '%Lookup Index Scans:%' THEN r := r || ',' || trim(l); END IF;
	END LOOP;
	RETURN r;
END $$;

CREATE FUNCTION same(q text) RETURNS TABLE (plan text, result_rows bigint, vs_no_pushdown bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
BEGIN
	plan := plan_of(q);
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS %s', q);
	SET LOCAL sift.agg_lookup_pushdown = off;
	EXECUTE format('CREATE TEMP TABLE r_no ON COMMIT DROP AS %s', q);
	RESET sift.agg_lookup_pushdown;
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', q);
	RESET sift.enable_agg;
	result_rows := (SELECT count(*) FROM r_on);
	vs_no_pushdown := (SELECT count(*) FROM ((TABLE r_on EXCEPT TABLE r_no) UNION ALL (TABLE r_no EXCEPT TABLE r_on)) d);
	vs_normal_plan := (SELECT count(*) FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d);
	RETURN NEXT;
END $$;

-- a group of an indexed membership table: the btree gives its rows; the same group through a table without index is filtered while loading
SELECT * FROM same('SELECT kind, count(*) AS n, sum(amount) AS s FROM ev WHERE dim_a_id IN (SELECT dim_a_id FROM dim_b WHERE group_id = 7) GROUP BY kind');
SELECT * FROM same('SELECT kind, count(*) AS n, sum(amount) AS s FROM ev WHERE dim_a_id IN (SELECT dim_a_id FROM dim_b WHERE group_id = 7 AND deleted_at IS NULL) GROUP BY kind');
SELECT * FROM same('SELECT kind, count(*) AS n, sum(amount) AS s FROM ev WHERE dim_a_id IN (SELECT dim_a_id FROM dim_b WHERE group_id = 7 AND role = 1) GROUP BY kind');
SELECT * FROM same('SELECT kind, count(*) AS n, sum(amount) AS s FROM ev WHERE dim_a_id IN (SELECT dim_a_id FROM members_bare WHERE group_id = 7 AND deleted_at IS NULL) GROUP BY kind');
SELECT * FROM same('SELECT kind, count(*) AS n, sum(amount) AS s FROM ev WHERE EXISTS (SELECT 1 FROM dim_b m WHERE m.dim_a_id = ev.dim_a_id AND m.group_id = 7) GROUP BY kind');
SELECT * FROM same('SELECT kind, count(*) AS n, sum(amount) AS s FROM ev WHERE NOT EXISTS (SELECT 1 FROM dim_b m WHERE m.dim_a_id = ev.dim_a_id AND m.group_id = 7) GROUP BY kind');
-- a group with a NULL key and a duplicate, two groups (no equality on the leading column: the table is read whole and filtered while loading), a group with no rows
SELECT * FROM same('SELECT kind, count(*) AS n FROM ev WHERE dim_a_id IN (SELECT dim_a_id FROM dim_b WHERE group_id = 3) GROUP BY kind');
SELECT * FROM same('SELECT kind, count(*) AS n FROM ev WHERE dim_a_id IN (SELECT dim_a_id FROM dim_b WHERE group_id IN (3, 4)) GROUP BY kind');
SELECT * FROM same('SELECT kind, count(*) AS n FROM ev WHERE dim_a_id IN (SELECT dim_a_id FROM dim_b WHERE group_id = 999) GROUP BY kind');
-- a value join on an attribute table
SELECT * FROM same('SELECT d.tier, count(*) AS n, sum(amount) AS s FROM ev JOIN dims d ON d.id = ev.dim_a_id WHERE d.country = 5 GROUP BY d.tier ORDER BY 1');
SELECT * FROM same('SELECT d.tier, count(*) AS n, sum(amount) AS s FROM ev JOIN dims d ON d.id = ev.dim_a_id WHERE d.country = 5 AND d.active GROUP BY d.tier ORDER BY 1');
-- no index on the condition: the filter runs while the table loads, and the answer is the normal plan's
SELECT * FROM same('SELECT d.country, count(*) AS n FROM ev JOIN dims d ON d.id = ev.dim_a_id WHERE d.tier = 2 GROUP BY d.country ORDER BY 1');

-- the value of the condition is a parameter, NULL included (no row matches NULL), and a generic plan
SET plan_cache_mode = force_generic_plan;
PREPARE by_group(int) AS SELECT kind, count(*) AS n FROM ev WHERE dim_a_id IN (SELECT dim_a_id FROM dim_b WHERE group_id = $1) GROUP BY kind ORDER BY kind;
EXECUTE by_group(7);
EXECUTE by_group(NULL);
EXECUTE by_group(999);
SET sift.enable_agg = off;
EXECUTE by_group(7);
RESET sift.enable_agg;
RESET plan_cache_mode;

DEALLOCATE ALL;
DROP FUNCTION same(text);
DROP FUNCTION plan_of(text);
DROP TABLE ev, dim_b, members_bare, dims;
DROP EXTENSION pg_sift;
