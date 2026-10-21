-- A FULL JOIN of the scanned table and a sub-select that computes a value PostgreSQL does not evaluate for the join's NULL rows (coalesce over the
-- sub-select's column, a placeholder): a scanned row without a match has NULL for it, not the value of the expression over NULL columns. The query
-- keeps its plan; the placeholder of the scanned table's own sub-select is NULL for the sub-select rows nothing matched, as for the other outer joins.
-- Only whether the results agree is printed.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE dims (id int PRIMARY KEY, name text, grp int);
INSERT INTO dims SELECT i, CASE WHEN i % 4 = 0 THEN NULL ELSE 'n' || i END, i % 3 FROM generate_series(1, 30) i;
CREATE TABLE facts (id int, dim_id int, v int);
INSERT INTO facts SELECT i, 1 + i % 40, i % 10 FROM generate_series(1, 3000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, dim_id, v);
VACUUM (FREEZE, ANALYZE) facts, dims;
CREATE FUNCTION outcome(q text, enabled bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE result text;
BEGIN
  PERFORM set_config('sift.enable_agg', enabled::text, true);
  PERFORM set_config(s, (NOT enabled)::text, true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
  EXECUTE 'SELECT coalesce(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text)::text, ''no rows'') FROM (' || q || ') t' INTO result;
  RETURN result;
EXCEPTION WHEN OTHERS THEN RETURN SQLSTATE || ' ' || SQLERRM;
END $$;
CREATE FUNCTION same(q text) RETURNS bool LANGUAGE sql AS $$ SELECT outcome(q, false) = outcome(q, true) $$;
SELECT same($q$
 SELECT d.nm, count(*) AS n, sum(f.v) AS total
 FROM facts f FULL JOIN (SELECT id, coalesce(name, 'x') AS nm FROM dims) d ON d.id = f.dim_id GROUP BY 1
$q$) AS lookup_value;
SELECT same($q$
 SELECT d.nm, d.grp, count(*) AS n, sum(f.v) AS total
 FROM facts f FULL JOIN (SELECT id, grp, coalesce(name, 'x') AS nm FROM dims) d ON d.id = f.dim_id GROUP BY ROLLUP (d.nm, d.grp)
$q$) AS lookup_value_rollup;
SELECT same($q$
 SELECT coalesce(d.name, 'x') AS nm, count(*) AS n, sum(f.v) AS total
 FROM facts f FULL JOIN dims d ON d.id = f.dim_id GROUP BY ROLLUP (coalesce(d.name, 'x'))
$q$) AS group_expression;
SELECT same($q$
 SELECT f.k, count(*) AS n, count(f.id) AS scanned
 FROM (SELECT id, dim_id, coalesce(v::text, 'x') AS k FROM facts) f FULL JOIN dims d ON d.id = f.dim_id GROUP BY 1
$q$) AS scanned_value;
DROP FUNCTION same(text), outcome(text, bool);
DROP TABLE facts, dims;
DROP EXTENSION pg_sift;
