-- A join whose dimension has a quarantined index: the fact index keeps its aggregate plan, the dimension is loaded from its heap, and the answers are those of the normal plan. A
-- quarantined fact index leaves the aggregate to PostgreSQL's plan
CREATE EXTENSION pg_sift_test CASCADE;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET sift.agg_force = on;
SET sift.backend_cache_kb = 0;
SET sift.shared_cache_kb = 0;
SET client_min_messages = error;
SET sift.on_corruption = quarantine;
CREATE FUNCTION expect(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
	RETURN SQLSTATE || ': ' || SQLERRM;
END $$;
CREATE FUNCTION answer(sql text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.enable_agg', scan::text, true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || sql || ') t' INTO r;
	RETURN r;
END $$;

CREATE TABLE ev (id int PRIMARY KEY, k int, v int) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, i % 5100, i % 11 FROM generate_series(1, 40000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, k, v);
CREATE TABLE dim (k int PRIMARY KEY, a int, b int) WITH (autovacuum_enabled = false);
INSERT INTO dim SELECT i, i % 7, i % 3 FROM generate_series(0, 4999) i;
CREATE INDEX dim_col ON dim USING sift_col (k, a, b);
VACUUM (FREEZE, ANALYZE) ev, dim;
CREATE TABLE shapes (n int, sql text);
INSERT INTO shapes VALUES
 (1, $q$SELECT dim.a, count(*), sum(ev.v) FROM ev JOIN dim ON dim.k = ev.k GROUP BY 1 ORDER BY 1$q$),
 (2, $q$SELECT dim.a, dim.b, count(*) FROM ev LEFT JOIN dim ON dim.k = ev.k GROUP BY 1, 2 ORDER BY 1, 2$q$),
 (3, $q$SELECT ev.id % 4, count(*), sum(ev.v) FROM ev WHERE ev.v > 3 GROUP BY 1 ORDER BY 1$q$);
SELECT n, answer(sql, true) = answer(sql, false) AS same_before FROM shapes ORDER BY n;

SELECT sift._test_poke('dim_col', 2, 8176, 9, 2);
SELECT expect($$ SELECT count(*) FROM dim WHERE a = 3 $$) AS detecting_statement;
SELECT index FROM sift.quarantine;
SELECT n, answer(sql, true) = answer(sql, false) AS same_dimension_out FROM shapes ORDER BY n;
SELECT n, expect(sql) AS statement FROM shapes ORDER BY n;

REINDEX INDEX dim_col;
SELECT sift._test_poke('ev_col', 2, 8176, 9, 2);
SELECT expect($$ SELECT count(*) FROM ev WHERE v = 3 $$) AS detecting_fact;
SELECT index FROM sift.quarantine;
SELECT n, answer(sql, true) = answer(sql, false) AS same_fact_out FROM shapes ORDER BY n;
SELECT n, expect(sql) AS statement FROM shapes ORDER BY n;
REINDEX INDEX ev_col;
SELECT count(*) AS listed_after_reindex FROM sift.quarantine;
SELECT n, answer(sql, true) = answer(sql, false) AS same_after FROM shapes ORDER BY n;

DROP TABLE ev, dim, shapes;
DROP FUNCTION expect(text), answer(text, bool);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
