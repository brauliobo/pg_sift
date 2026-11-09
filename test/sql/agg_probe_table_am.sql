-- A correlated probe of a table of another access method (sift_table) fetches every entry through the access method, an Index Only Scan too: the scan that probes it does the work of
-- the normal plan's nested loop and no less, so it is not chosen; the same probe of a heap table, whose entries an all-visible page gives from the btree, is. The answers are the same.
-- A NOT EXISTS (shapes 1 and 3 on the table of the other access method) is counted from a scan of the fact table instead (agg/flat/invert.c), which reads less than the probes.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;

CREATE TABLE dim (id int PRIMARY KEY, grp int) WITH (autovacuum_enabled = false);
INSERT INTO dim SELECT g, g % 7 FROM generate_series(1, 5000) g;
CREATE INDEX dim_col ON dim USING sift_col (id, grp);
-- the keys 4501 to 5000 have no fact
CREATE TABLE fact_am (id bigint PRIMARY KEY, dim_id int) USING sift_table;
INSERT INTO fact_am SELECT g, 1 + (hashint4(g) & 2147483647) % 4500 FROM generate_series(1, 100000) g;
CREATE INDEX fact_am_k ON fact_am (dim_id);
CREATE TABLE fact_h (id bigint PRIMARY KEY, dim_id int) WITH (autovacuum_enabled = false);
INSERT INTO fact_h SELECT * FROM fact_am;
CREATE INDEX fact_h_k ON fact_h (dim_id);
VACUUM (FREEZE, ANALYZE) dim, fact_am, fact_h;

CREATE TABLE shapes (n int, sql text);
INSERT INTO shapes
SELECT row_number() OVER (), format(q, t) FROM (VALUES
 ($q$SELECT count(*) FROM dim d WHERE NOT EXISTS (SELECT 1 FROM %s f WHERE f.dim_id = d.id)$q$),
 ($q$SELECT count(*) FROM dim d WHERE EXISTS (SELECT 1 FROM %s f WHERE f.dim_id = d.id)$q$),
 ($q$SELECT d.grp, count(*) FROM dim d WHERE NOT EXISTS (SELECT 1 FROM %s f WHERE f.dim_id = d.id) GROUP BY 1 ORDER BY 1$q$)
) q (q), (VALUES ('fact_am'), ('fact_h')) t (t);

CREATE FUNCTION answer(sql text, force bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.agg_force', force::text, true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || sql || ') t' INTO r;
	RETURN r;
END $$;
CREATE FUNCTION plan(sql text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal'; BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || sql LOOP
		IF l ~ 'Custom Scan \(SiftAgg\)' THEN r := 'SiftAgg'; END IF;
	END LOOP;
	RETURN r;
END $$;

SELECT n, substring(sql from 'FROM (fact_\w+)') AS fact, plan(sql), answer(sql, true) = answer(sql, false) AS same_answers FROM shapes ORDER BY n;

DROP FUNCTION plan(text), answer(text, bool);
DROP TABLE shapes, dim, fact_am, fact_h;
DROP EXTENSION pg_sift;
