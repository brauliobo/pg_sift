-- A column of the joined table that is an expression (lower(name), n * 2) is read from the leaf of an index that holds it, or computed from the leaves of the columns it reads
-- when the index chosen for the load lacks it (upper(name) and n + 1 are in no index). A dense table (classes of the keys) keeps a computed column as the outcome of a test, so only a boolean one: a text or an integer
-- expression is read as rows. The index chosen is the first by OID that holds the columns, here the one without the expressions. The answers are those of the normal plan.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET sift.agg_lookup_cache_kb = 0;
SET sift.agg_lookup_shared_kb = 0;
SET client_min_messages = warning;

CREATE TABLE cv_dim (id int PRIMARY KEY, name text, n int) WITH (autovacuum_enabled = false);
INSERT INTO cv_dim SELECT g, CASE g % 4 WHEN 0 THEN 'Alpha' WHEN 1 THEN 'BETA' WHEN 2 THEN 'Gamma' ELSE 'delta' END, g % 10 FROM generate_series(1, 5000) g;
CREATE TABLE cv_fact (id int, dim_id int, v int) WITH (autovacuum_enabled = false);
INSERT INTO cv_fact SELECT g, 1 + g % 5000, g % 100 FROM generate_series(1, 60000) g;
CREATE INDEX cv_dim_a ON cv_dim USING sift_col (id, name, n);
CREATE INDEX cv_dim_b ON cv_dim USING sift_col (id, lower(name), (n * 2));
CREATE INDEX cv_fact_col ON cv_fact USING sift_col (id, dim_id, v);
RESET client_min_messages;
VACUUM (FREEZE, ANALYZE) cv_dim, cv_fact;

CREATE TABLE shapes (n int, sql text);
INSERT INTO shapes VALUES
 (1, $q$SELECT lower(d.name), count(*), sum(f.v) FROM cv_fact f JOIN cv_dim d ON d.id = f.dim_id GROUP BY 1$q$),
 (2, $q$SELECT d.n * 2, count(*), sum(f.v) FROM cv_fact f JOIN cv_dim d ON d.id = f.dim_id GROUP BY 1$q$),
 (3, $q$SELECT lower(d.name), d.n * 2, count(*) FROM cv_fact f JOIN cv_dim d ON d.id = f.dim_id GROUP BY 1, 2$q$),
 (4, $q$SELECT lower(d.name) = 'beta', d.n * 2 > 6, count(*) FROM cv_fact f JOIN cv_dim d ON d.id = f.dim_id GROUP BY 1, 2$q$),
 (5, $q$SELECT count(*) FILTER (WHERE lower(d.name) = 'beta'), sum(f.v) FILTER (WHERE d.n * 2 > 6) FROM cv_fact f JOIN cv_dim d ON d.id = f.dim_id$q$),
 (6, $q$SELECT upper(d.name), count(*), sum(f.v) FROM cv_fact f JOIN cv_dim d ON d.id = f.dim_id GROUP BY 1$q$),
 (7, $q$SELECT d.n + 1, lower(d.name) = 'beta', count(*) FROM cv_fact f JOIN cv_dim d ON d.id = f.dim_id GROUP BY 1, 2$q$);

CREATE FUNCTION answer(sql text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.agg_force', scan::text, true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || sql || ') t' INTO r;
	RETURN r;
END $$;

SELECT n, answer(sql, true) = answer(sql, false) AS same_answers FROM shapes ORDER BY n;

DROP FUNCTION answer(text, bool);
DROP TABLE shapes, cv_fact, cv_dim;
DROP EXTENSION pg_sift;
