-- The dimension map of a dimension whose index keeps a journal of its changed keys (a child index is connected to it): the map is stamped with the horizon of the snapshot it
-- was read under and a statement reads it when no row was inserted into the dimension since and no key was changed in it, so a write that nobody has vacuumed away only
-- makes the next statement read the leaves again and keep a map of its own, where the pages of the table are the map's state otherwise. The answers are those of the normal plan.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET sift.agg_force = on;
SET sift.agg_lookup_cache_kb = 0;
SET sift.agg_lookup_shared_kb = 0;

CREATE TABLE dim (k int PRIMARY KEY, a int, b int, flag bool) WITH (autovacuum_enabled = false);
INSERT INTO dim SELECT i, i % 7, i % 3, i % 5 = 0 FROM generate_series(0, 4999) i;
CREATE INDEX dim_col ON dim USING sift_col (k, a, b, flag);
CREATE TABLE ev (id int PRIMARY KEY, k int, v int) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, CASE WHEN i % 211 = 0 THEN NULL ELSE i % 5100 END, i % 11 FROM generate_series(1, 40000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, k, v) WITH (connect = 'k -> dim(k)');
VACUUM (FREEZE, ANALYZE) ev, dim;

CREATE TABLE shapes (n int, sql text);
INSERT INTO shapes VALUES
 (1, $q$SELECT dim.a, count(*), sum(ev.v) FROM ev JOIN dim ON dim.k = ev.k GROUP BY 1 ORDER BY 1$q$),
 (2, $q$SELECT dim.a, dim.b, count(*) FROM ev LEFT JOIN dim ON dim.k = ev.k GROUP BY 1, 2 ORDER BY 1, 2$q$),
 (3, $q$SELECT ev.id % 4, sum(dim.a), count(dim.flag), max(dim.b), count(DISTINCT ev.k) FROM ev JOIN dim ON dim.k = ev.k GROUP BY 1 ORDER BY 1$q$);

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
CREATE FUNCTION report(stage text) RETURNS TABLE (stage text, shape int, same_answers bool, shared bool, built bool) LANGUAGE sql AS $$
	SELECT stage, n, answer(sql, true) = answer(sql, false), m.shared > 0, m.built > 0 FROM shapes, LATERAL maps(sql) m ORDER BY n
$$;

-- the first statement of a shape reads the leaves and keeps the map; the next ones share it
SELECT * FROM report('first');
SELECT * FROM report('shared');

-- a committed update, never vacuumed: the first statement after it reads the leaves again, the next ones share that map
UPDATE dim SET a = 6, flag = NULL WHERE k % 31 = 0;
SELECT * FROM report('after an update');
SELECT * FROM report('shared again');

-- a committed insert (events of these keys had no row), and a committed delete
INSERT INTO dim SELECT i, 1, 1, true FROM generate_series(5000, 5049) i;
SELECT * FROM report('after an insert');
DELETE FROM dim WHERE k % 53 = 0;
SELECT * FROM report('after a delete');
SELECT * FROM report('shared after the delete');

-- the reader's own transaction: its changes are in the answers and the map it makes is nobody else's
BEGIN;
UPDATE dim SET b = 2 WHERE k % 17 = 0;
SELECT * FROM report('in the transaction');
SELECT * FROM report('in the transaction again');
ROLLBACK;
SELECT * FROM report('after the rollback');
SELECT * FROM report('shared after the rollback');

-- the connection switched off: the journal of the dimension stays, and so does its stamp
ALTER INDEX ev_col SET (connect = 'off');
REINDEX INDEX ev_col;
UPDATE dim SET a = 5 WHERE k = 11;
SELECT * FROM report('connection off, written');
SELECT * FROM report('connection off, shared');
VACUUM (INDEX_CLEANUP ON) dim;
SELECT * FROM report('connection off, vacuumed');

DROP FUNCTION report(text), maps(text), answer(text, bool);
DROP TABLE shapes, ev, dim;
DROP EXTENSION pg_sift;
