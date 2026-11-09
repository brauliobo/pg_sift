-- A child index with several connections (one for each foreign key column): each dimension has its own journal and its own maps, so a write to one dimension makes the
-- statements that join it read its leaves again and leaves the maps of the others shared. The answers are those of the normal plan.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET sift.agg_force = on;
SET sift.agg_lookup_cache_kb = 0;
SET sift.agg_lookup_shared_kb = 0;

CREATE TABLE dim_a (k int PRIMARY KEY, a int, b int) WITH (autovacuum_enabled = false);
INSERT INTO dim_a SELECT i, i % 7, i % 3 FROM generate_series(0, 4999) i;
CREATE INDEX dim_a_col ON dim_a USING sift_col (k, a, b);
CREATE TABLE dim_b (k int PRIMARY KEY, x int, y int) WITH (autovacuum_enabled = false);
INSERT INTO dim_b SELECT i, i % 5, i % 11 FROM generate_series(0, 2999) i;
CREATE INDEX dim_b_col ON dim_b USING sift_col (k, x, y);
CREATE TABLE ev (id int PRIMARY KEY, ka int, kb int, v int) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, i % 5100, i % 3100, i % 13 FROM generate_series(1, 40000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, ka, kb, v) WITH (connect = 'ka -> dim_a(k) carries a, b; kb -> dim_b(k) carries x, y');
VACUUM (FREEZE, ANALYZE) ev, dim_a, dim_b;

CREATE TABLE shapes (n int, sql text);
INSERT INTO shapes VALUES
 (1, $q$SELECT dim_a.a, count(*), sum(ev.v) FROM ev JOIN dim_a ON dim_a.k = ev.ka GROUP BY 1 ORDER BY 1$q$),
 (2, $q$SELECT dim_b.x, count(*), sum(ev.v) FROM ev JOIN dim_b ON dim_b.k = ev.kb GROUP BY 1 ORDER BY 1$q$),
 (3, $q$SELECT dim_a.b, dim_b.y, count(*) FROM ev JOIN dim_a ON dim_a.k = ev.ka JOIN dim_b ON dim_b.k = ev.kb GROUP BY 1, 2 ORDER BY 1, 2$q$);

CREATE FUNCTION answer(sql text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.enable_agg', scan::text, true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || sql || ') t' INTO r;
	RETURN r;
END $$;
-- the maps the statement shared and the ones it made, from EXPLAIN ANALYZE
CREATE FUNCTION maps(sql text, OUT shared int, OUT built int) LANGUAGE plpgsql AS $$
DECLARE l text; BEGIN
	shared := 0; built := 0;
	PERFORM set_config('sift.enable_agg', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) ' || sql LOOP
		IF l ~ '(\d+) maps shared' THEN shared := substring(l from '(\d+) maps shared')::int; END IF;
		IF l ~ '(\d+) maps built' THEN built := substring(l from '(\d+) maps built')::int; END IF;
	END LOOP;
END $$;
CREATE FUNCTION report(stage text) RETURNS TABLE (stage text, shape int, same_answers bool, shared int, built int) LANGUAGE sql AS $$
	SELECT stage, n, answer(sql, true) = answer(sql, false), m.shared, m.built FROM shapes, LATERAL maps(sql) m ORDER BY n
$$;

-- every dimension of a statement makes its map once; the next statements share them
SELECT * FROM report('first');
SELECT * FROM report('shared');

-- a write to one dimension: the statements that join it read its leaves again, the other dimension's map stays shared
UPDATE dim_a SET a = 6 WHERE k % 31 = 0;
SELECT * FROM report('dimension a written');
SELECT * FROM report('dimension a shared again');
UPDATE dim_b SET y = 10 WHERE k % 29 = 0;
SELECT * FROM report('dimension b written');
SELECT * FROM report('dimension b shared again');

-- the second connection switched off: the journal of dimension b stays
ALTER INDEX ev_col SET (connect = 'ka -> dim_a(k) carries a, b');
REINDEX INDEX ev_col;
UPDATE dim_b SET x = 4 WHERE k = 7;
SELECT * FROM report('connection b off, written');
SELECT * FROM report('connection b off, shared');

DROP FUNCTION report(text), maps(text), answer(text, bool);
DROP TABLE shapes, ev, dim_a, dim_b;
DROP EXTENSION pg_sift;
