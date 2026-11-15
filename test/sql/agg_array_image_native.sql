-- A test of the elements of an array column that the index holds as whole arrays only (paths = 'auto:') is run row by row: each row looks its image up in the
-- batch and resolves its entry, 170 instructions a row whatever the number of distinct arrays, where the GIN bitmap of the normal plan answers a containment
-- of a few rows in a fraction of that. The planner prices the row, so the containment keeps the normal plan, while the tests that read the whole table anyway
-- (= ANY with a range, unnest) stay on the index; with the element lists beside the arrays (paths = '+tags[] int4') the containment is a test of the lists
-- and goes to the index. The rows are those of the normal plan in every case, also when the scan is forced on the images.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE ev (id int PRIMARY KEY, dim_a_id int, d date, tags int[], amount float8);
INSERT INTO ev
SELECT g, 1 + g % 300, date '2025-01-01' + g % 400, CASE WHEN g % 5 <> 0 THEN ARRAY[g % 21, (g * 7) % 21] END, g % 100
FROM generate_series(1, 100000) g;
CREATE INDEX ev_gin ON ev USING gin (tags);
CREATE INDEX ev_col ON ev USING sift_col (id, dim_a_id, d, tags, amount) WITH (paths = 'auto:');
VACUUM (FREEZE, ANALYZE) ev;

CREATE TABLE shapes (n int, q text);
INSERT INTO shapes VALUES
	(1, 'SELECT count(*) FROM ev WHERE tags @> ARRAY[5]'),
	(2, 'SELECT count(*) FROM ev WHERE tags @> ARRAY[3, 7]'),
	(3, 'SELECT count(*) FROM ev WHERE 7 = ANY (tags) AND d >= ''2025-06-01'''),
	(4, 'SELECT t, count(*) FROM ev, unnest(tags) t WHERE dim_a_id <= 100 GROUP BY 1 ORDER BY 1');

CREATE FUNCTION rows_md5(q text, agg boolean) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', CASE WHEN agg THEN 'on' ELSE 'off' END, true);
	PERFORM set_config('sift.agg_force', CASE WHEN agg THEN 'on' ELSE 'off' END, true);
	EXECUTE format('SELECT md5(string_agg(x::text, '','' ORDER BY x::text)) FROM (%s) x', q) INTO r;
	RETURN r;
END $$;

-- the arrays only: the containments keep the normal plan (the slug is the cost), = ANY and unnest are on the index
SELECT n, w.used, w.slug, w.used = (n IN (3, 4)) AS as_priced, rows_md5(q, true) = rows_md5(q, false) AS same_rows
FROM shapes, LATERAL sift.explain_why(q) w ORDER BY n;

-- the lists beside the arrays: all four on the index
ALTER INDEX ev_col SET (paths = '+tags[] int4');
REINDEX INDEX ev_col;
SELECT n, w.used, w.slug, rows_md5(q, true) = rows_md5(q, false) AS same_rows
FROM shapes, LATERAL sift.explain_why(q) w ORDER BY n;

DROP FUNCTION rows_md5(text, boolean);
DROP TABLE shapes, ev;
DROP EXTENSION pg_sift;
