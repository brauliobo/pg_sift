-- The cast of a JSON null to a number or boolean is NULL in PostgreSQL 18 and an error before it. The scan reads the NULL slot, so before 18 a cast of a
-- leaf that may hold a JSON null keeps the normal plan (which raises), while a leaf without one, and `->>` and its cast (NULL in every version), keep the scan.
-- Every result below is the same with the scan off and on; the columns say what differs by version: `scan` (pushed as the version allows) and `raises`.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE docs (id int, doc jsonb);
INSERT INTO docs
SELECT i, CASE i % 5 WHEN 0 THEN '{"k": null, "c": 1}'::jsonb WHEN 1 THEN '{"k": 2.5, "c": 2}'::jsonb WHEN 2 THEN '{"c": 3}'::jsonb ELSE jsonb_build_object('k', i % 7, 'c', i % 4) END
FROM generate_series(1, 20000) i;
CREATE INDEX docs_col ON docs USING sift_col (id, doc) WITH (paths = 'doc.k float8, doc.c float8');
CREATE TABLE dim_b AS TABLE docs;
CREATE INDEX members_col ON dim_b USING sift_col (id, doc) WITH (paths = 'doc.* float8');
VACUUM (FREEZE, ANALYZE) docs;
VACUUM (FREEZE, ANALYZE) dim_b;
-- The result, or the SQLSTATE, with the scan off and on. on: the table scans are off, so a Custom Scan (SiftAgg) runs when the query is one it can run.
CREATE FUNCTION outcome(q text, enabled bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE result text;
BEGIN
  PERFORM set_config('sift.enable_agg', enabled::text, true);
  PERFORM set_config(s, (NOT enabled)::text, true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
  EXECUTE 'SELECT coalesce(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text)::text, ''no rows'') FROM (' || q || ') t' INTO result;
  RETURN result;
EXCEPTION WHEN OTHERS THEN RETURN 'error ' || SQLSTATE;
END $$;
CREATE FUNCTION runs_in_scan(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE line text; found bool := false;
BEGIN
  PERFORM set_config(s, 'false', true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
  FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP found := found OR line LIKE '%Custom Scan (SiftAgg)%'; END LOOP;
  RETURN found;
END $$;
-- jnull: the query casts a leaf that holds a JSON null. scan: it runs in the scan, which it does unless that cast raises before 18; raises: it fails, as before 18 it must
CREATE FUNCTION check_query(name text, q text, jnull bool) RETURNS TABLE (label text, scan bool, same bool, raises bool) LANGUAGE plpgsql AS $$
DECLARE old bool := current_setting('server_version_num')::int < 180000;
BEGIN
  RETURN QUERY SELECT name, runs_in_scan(q) = NOT (jnull AND old), outcome(q, false) = outcome(q, true), (outcome(q, true) = 'error 22023') = (jnull AND old);
END $$;
SELECT * FROM check_query('cast of a path', $q$SELECT id % 3 AS g, count(*) AS n, sum((doc -> 'k')::float8) AS total FROM docs GROUP BY 1$q$, true);
SELECT * FROM check_query('cast of a member', $q$SELECT r.key, count(*) AS n, sum(CAST(r.value AS float)) AS total FROM dim_b, jsonb_each(doc) r GROUP BY 1$q$, true);
SELECT * FROM check_query('integer cast of a path', $q$SELECT id % 3 AS g, sum((doc -> 'k')::int) AS total FROM docs GROUP BY 1$q$, true);
-- the text of the value is NULL for a JSON null in every version
SELECT * FROM check_query('text cast of a path', $q$SELECT count(*) AS n, sum((doc ->> 'k')::float8) AS total FROM docs$q$, false);
-- a leaf without a JSON null (a missing key is a NULL that raises nowhere) keeps the scan
SELECT * FROM check_query('cast of a leaf without one', $q$SELECT id % 3 AS g, count(*) AS n, sum((doc -> 'c')::float8) AS total FROM docs GROUP BY 1$q$, false);
SELECT * FROM check_query('integer cast, leaf without one', $q$SELECT id % 3 AS g, sum((doc -> 'c')::int) AS total FROM docs GROUP BY 1$q$, false);
-- a write buffer keeps which leaves hold a JSON null: rows without one keep the scan, one with it is a cast that raises
CREATE TABLE late (id int, doc jsonb);
INSERT INTO late SELECT i, jsonb_build_object('c', i % 4) FROM generate_series(1, 20000) i;
CREATE INDEX late_col ON late USING sift_col (id, doc) WITH (paths = 'doc.c float8');
VACUUM (FREEZE, ANALYZE) late;
SELECT * FROM check_query('segments only', $q$SELECT count(*) AS n, sum((doc -> 'c')::float8) AS total FROM late$q$, false);
INSERT INTO late VALUES (20001, '{"c": 5}');
SELECT * FROM check_query('clean write buffer', $q$SELECT count(*) AS n, sum((doc -> 'c')::float8) AS total FROM late$q$, false);
INSERT INTO late VALUES (20002, '{"c": null}');
SELECT * FROM check_query('write buffer', $q$SELECT count(*) AS n, sum((doc -> 'c')::float8) AS total FROM late$q$, true);
SELECT sift.col_promote('late_col');
SELECT * FROM check_query('promoted', $q$SELECT count(*) AS n, sum((doc -> 'c')::float8) AS total FROM late$q$, true);
-- the zones the flag is in are the chunks' own
SELECT sift.col_verify('docs_col'), sift.col_verify('late_col');
-- a sift_table: the same, its write buffer included
CREATE TABLE tdocs (id int, doc jsonb) USING sift_table;
SELECT sift.table_set_paths('tdocs', 'doc', 'doc.k float8');
INSERT INTO tdocs SELECT id, doc FROM docs;
VACUUM tdocs;
SELECT * FROM check_query('table', $q$SELECT id % 3 AS g, count(*) AS n, sum((doc -> 'k')::float8) AS total FROM tdocs GROUP BY 1$q$, true);
DROP FUNCTION check_query(text, text, bool), outcome(text, bool), runs_in_scan(text);
DROP TABLE docs, dim_b, late, tdocs;
DROP EXTENSION pg_sift;
