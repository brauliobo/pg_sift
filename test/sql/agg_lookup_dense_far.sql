-- A table kept as classes of its keys whose keys outgrow the cache (more than 524,288 of them) is read with the line of each key asked for ahead: the answers are those of the
-- normal plan, NULL keys and keys the table does not have included, and the table is classed.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;

CREATE TABLE far_dim (k int PRIMARY KEY, a smallint, b smallint) WITH (autovacuum_enabled = false);
INSERT INTO far_dim SELECT g, g % 5, g % 3 FROM generate_series(1, 600000) g;
CREATE TABLE far_fact (id int, k int, v int) WITH (autovacuum_enabled = false);
-- one key in 997 is NULL, one in 1,009 is a key the dimension does not have
INSERT INTO far_fact SELECT g, CASE WHEN g % 997 = 0 THEN NULL WHEN g % 1009 = 0 THEN 700000 + g ELSE 1 + (hashint4(g) & 2147483647) % 600000 END, g % 100
FROM generate_series(1, 400000) g;
CREATE INDEX far_dim_col ON far_dim USING sift_col (k, a, b);
CREATE INDEX far_fact_col ON far_fact USING sift_col (id, k, v);
VACUUM (FREEZE, ANALYZE) far_dim, far_fact;

CREATE TABLE shapes (n int, sql text);
INSERT INTO shapes VALUES
 (1, $q$SELECT d.a, count(*), sum(f.v) FROM far_fact f LEFT JOIN far_dim d ON d.k = f.k GROUP BY 1$q$),
 (2, $q$SELECT d.a, d.b, count(*) FROM far_fact f JOIN far_dim d ON d.k = f.k GROUP BY 1, 2$q$),
 (3, $q$SELECT d.b, count(f.v), sum(f.v) FROM far_fact f LEFT JOIN far_dim d ON d.k = f.k WHERE f.v > 10 GROUP BY 1$q$);

CREATE FUNCTION answer(sql text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.agg_force', scan::text, true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || sql || ') t' INTO r;
	RETURN r;
END $$;
CREATE FUNCTION classed(sql text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; BEGIN
	PERFORM set_config('sift.agg_force', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) ' || sql LOOP
		IF l ~ 'Lookup: .*[1-9][0-9]* (rows classed|cached tables)' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

SELECT n, classed(sql), answer(sql, true) = answer(sql, false) AS same_answers FROM shapes ORDER BY n;

DROP FUNCTION answer(text, bool), classed(text);
DROP TABLE shapes, far_fact, far_dim;
DROP EXTENSION pg_sift;
