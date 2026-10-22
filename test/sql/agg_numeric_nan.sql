-- NaN in a numeric(p, s) column of a sift_col index: SiftAgg gives the rows of the normal plan for every shape that reads the column, with
-- PostgreSQL's order of numeric (NaN equals NaN and is greater than every number): extremes, sums, averages, counts, DISTINCT, keys,
-- ORDER BY, comparisons with numbers, with NaN and between two expressions, expressions, float8 casts. The rows are in segments, in the
-- write buffer, and in the segments after a promotion and a merge; a cast of a NaN to an integer raises PostgreSQL's error.
CREATE EXTENSION pg_sift;
LOAD 'pg_sift';
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
SET random_page_cost = 10000;

CREATE TABLE nn (id int PRIMARY KEY, g int, n numeric(12,2), m numeric(10,3)) WITH (autovacuum_enabled = false);
INSERT INTO nn
SELECT i, i % 7,
	   CASE WHEN i % 97 = 0 THEN 'NaN' WHEN i % 11 = 0 THEN NULL ELSE round(((i * 37) % 4001 - 2000) / 7.0, 2) END,
	   CASE WHEN i % 89 = 0 THEN 'NaN' WHEN i % 13 = 0 THEN NULL ELSE round(((i * 53) % 3001 - 1500) / 11.0, 3) END
FROM generate_series(1, 6000) i;
-- groups of NaN only, of NaN and NULL, of NULL only
INSERT INTO nn VALUES (7001, 9, 'NaN', 1), (7002, 9, 'NaN', 'NaN'), (7003, 10, 'NaN', NULL), (7004, 10, NULL, 2), (7005, 11, NULL, NULL);
CREATE INDEX nn_col ON nn USING sift_col (id, g, n, m);
VACUUM (FREEZE, ANALYZE) nn;

CREATE TABLE shapes (id serial PRIMARY KEY, shape text, q text);
INSERT INTO shapes (shape, q) VALUES
	('extremes', 'SELECT min(n) AS lo, max(n) AS hi, count(n) AS c, count(*) AS r, count(DISTINCT n) AS d FROM nn'),
	('sums', 'SELECT sum(n) AS s, avg(n) AS a, sum(DISTINCT n) AS sd FROM nn'),
	('by_group', 'SELECT g, min(n) AS lo, max(n) AS hi, sum(n) AS s, avg(n) AS a, count(n) AS c, count(DISTINCT n) AS d FROM nn GROUP BY g'),
	('key', 'SELECT n, count(*) AS c FROM nn GROUP BY n'),
	('keys', 'SELECT g, n, count(*) AS c, max(m) AS hi FROM nn GROUP BY g, n'),
	('expr_aggs', 'SELECT min(n + 1) AS a, max(n * 2) AS b, sum(n - m) AS c, avg(abs(-n)) AS d, max(round(n, 1)) AS e, min(floor(n)) AS f, max(ceil(n)) AS h FROM nn'),
	('expr_by_group', 'SELECT g, max(n + 1) AS a, sum(n * m) AS b, min(trunc(n, 1)) AS c FROM nn GROUP BY g'),
	('expr_key', 'SELECT n + m AS k, count(*) AS c FROM nn GROUP BY 1'),
	('expr_distinct', 'SELECT g, count(DISTINCT n + m) AS d, count(DISTINCT n * 0) AS z FROM nn GROUP BY g'),
	('filters', 'SELECT count(*) FILTER (WHERE n > 5) AS gt, count(*) FILTER (WHERE n >= 5) AS ge, count(*) FILTER (WHERE n < 5) AS lt, count(*) FILTER (WHERE n <= 5) AS le, count(*) FILTER (WHERE n = 5.5) AS eq, count(*) FILTER (WHERE n <> 5.5) AS ne, count(*) FILTER (WHERE n IS NULL) AS isnull, count(*) FILTER (WHERE n IS NOT NULL) AS notnull FROM nn'),
	('nan_filters', 'SELECT count(*) FILTER (WHERE n > ''NaN'') AS gt, count(*) FILTER (WHERE n >= ''NaN'') AS ge, count(*) FILTER (WHERE n = ''NaN'') AS eq, count(*) FILTER (WHERE n < ''NaN'') AS lt, count(*) FILTER (WHERE n <= ''NaN'') AS le, count(*) FILTER (WHERE n <> ''NaN'') AS ne FROM nn'),
	('inf_filters', 'SELECT count(*) FILTER (WHERE n < ''Infinity'') AS lt, count(*) FILTER (WHERE n > ''-Infinity'') AS gt, count(*) FILTER (WHERE n = ''Infinity'') AS eq FROM nn'),
	('two_sided', 'SELECT count(*) FILTER (WHERE n > m) AS gt, count(*) FILTER (WHERE n >= m) AS ge, count(*) FILTER (WHERE n = m) AS eq, count(*) FILTER (WHERE n < m) AS lt, count(*) FILTER (WHERE n <= m) AS le, count(*) FILTER (WHERE n <> m) AS ne FROM nn'),
	('expr_filters', 'SELECT count(*) FILTER (WHERE n + 1 > 5) AS a, count(*) FILTER (WHERE n * 2 = ''NaN'') AS b, count(*) FILTER (WHERE n + 1 <= ''NaN'') AS c, count(*) FILTER (WHERE n - 1 < ''Infinity'') AS d, count(*) FILTER (WHERE n + 1 > m * 2) AS e, count(*) FILTER (WHERE abs(n) <> m) AS f FROM nn'),
	('where_gt', 'SELECT g, count(*) AS c, sum(n) AS s, max(n) AS hi FROM nn WHERE n > 5 GROUP BY g'),
	('where_eq_nan', 'SELECT g, count(*) AS c, max(m) AS hi FROM nn WHERE n = ''NaN'' GROUP BY g'),
	('where_lt', 'SELECT g, count(*) AS c, min(n) AS lo FROM nn WHERE n < 5 GROUP BY g'),
	('where_ne', 'SELECT g, count(*) AS c FROM nn WHERE n <> 5.5 AND m >= 1 GROUP BY g'),
	('where_not', 'SELECT g, count(*) AS c FROM nn WHERE NOT (n > 5) GROUP BY g'),
	('where_or', 'SELECT g, count(*) AS c, sum(m) AS s FROM nn WHERE n > 5 OR m < 0 GROUP BY g'),
	('where_pair', 'SELECT g, count(*) AS c FROM nn WHERE n > m GROUP BY g'),
	('having', 'SELECT g, max(n) AS hi FROM nn GROUP BY g HAVING max(n) > 5'),
	('order_desc', 'SELECT id, n FROM nn ORDER BY n DESC, id LIMIT 20'),
	('order_asc', 'SELECT id, n FROM nn ORDER BY n ASC NULLS LAST, id LIMIT 20'),
	('order_where', 'SELECT id, n, m FROM nn WHERE g = 3 ORDER BY n DESC NULLS LAST, id LIMIT 10'),
	('in_between', 'SELECT count(*) FILTER (WHERE n IN (1.5, ''NaN'')) AS a, count(*) FILTER (WHERE n BETWEEN -5 AND 5) AS b, count(*) FILTER (WHERE n NOT IN (1.5, 2)) AS c FROM nn'),
	('filtered_aggs', 'SELECT g, min(n) FILTER (WHERE m > 0) AS a, max(n) FILTER (WHERE m = ''NaN'') AS b, sum(n) FILTER (WHERE n <> ''NaN'') AS c FROM nn GROUP BY g'),
	('having_sum', 'SELECT g, sum(n + 1) FILTER (WHERE n + 1 > m) AS a FROM nn GROUP BY g HAVING sum(n) > 1 OR max(n) > 5'),
	('rollup', 'SELECT n, count(*) AS c FROM nn GROUP BY ROLLUP (n)'),
	('generic', 'SELECT g, array_agg(n ORDER BY id) AS a, string_agg(n::text, '','' ORDER BY id) AS s FROM nn GROUP BY g'),
	('percentile', 'SELECT g, percentile_disc(0.5) WITHIN GROUP (ORDER BY n) AS p, mode() WITHIN GROUP (ORDER BY n) AS m FROM nn GROUP BY g'),
	('floats', 'SELECT g, sum(n::float8) AS s, avg(n::float8) AS a, max(n::float8) AS hi FROM nn GROUP BY g'),
	('combos', 'SELECT g, n % 3 AS k, sum(n / 3) AS s, count(*) FILTER (WHERE n > 1.5::float8) AS c FROM nn GROUP BY 1, 2');

\ir test/agg_helpers.sql

-- the rows with and without the scan are the same (and not empty)
CREATE FUNCTION same_rows(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a bigint; n bigint;
BEGIN
	SET LOCAL sift.enable_agg = on;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	SET LOCAL sift.enable_agg = on;
	SELECT count(*) INTO a FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d;
	SELECT count(*) INTO n FROM r_on;
	DROP TABLE r_on, r_off;
	RETURN a = 0 AND n > 0;
END $$;

-- the order of a LIMIT is the query's: the rows are compared as they come
CREATE FUNCTION same_ordered(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a text[]; b text[];
BEGIN
	SET LOCAL sift.enable_agg = on;
	EXECUTE format('SELECT array_agg(t::text) FROM (%s) t', q) INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('SELECT array_agg(t::text) FROM (%s) t', q) INTO b;
	SET LOCAL sift.enable_agg = on;
	RETURN a = b AND cardinality(a) > 0;
END $$;

-- the error of a query with and without the scan (or none)
CREATE FUNCTION error_of(q text, scan boolean) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('SET LOCAL sift.enable_agg = %s', scan);
	EXECUTE q;
	RETURN 'no error';
EXCEPTION WHEN OTHERS THEN
	RETURN SQLSTATE || ' ' || SQLERRM;
END $$;

SELECT shape, pushed(q), same_rows(q) AS same FROM shapes WHERE shape NOT LIKE 'order%' ORDER BY id;
SELECT shape, pushed(q), same_ordered(q) AS same FROM shapes WHERE shape LIKE 'order%' ORDER BY id;
SELECT pushed('SELECT sum(n::int) FROM nn') AS cast_pushed, pushed('SELECT sum(n::int) FROM nn WHERE n <> ''NaN''') AS filtered_pushed;
SELECT error_of('SELECT sum(n::int) FROM nn', true) = error_of('SELECT sum(n::int) FROM nn', false) AS cast_error,
       error_of('SELECT sum(n::int) FROM nn', true) AS message;
SELECT error_of('SELECT sum(n::int) FROM nn WHERE n <> ''NaN''', true) AS filtered_cast,
       error_of('SELECT sum(n::int) FROM nn WHERE n <> ''NaN''', false) AS plain_cast;

-- rows in the write buffer: NaN among them, in groups that have none and in ones that do
INSERT INTO nn
SELECT 10000 + i, i % 7, CASE WHEN i % 5 = 0 THEN 'NaN' WHEN i % 7 = 0 THEN NULL ELSE round(((i * 29) % 301 - 100) / 3.0, 2) END,
       CASE WHEN i % 6 = 0 THEN 'NaN' ELSE round((i % 40) / 4.0, 3) END
FROM generate_series(1, 300) i;
SELECT shape, pushed(q), same_rows(q) AS same FROM shapes WHERE shape NOT LIKE 'order%' ORDER BY id;
SELECT shape, pushed(q), same_ordered(q) AS same FROM shapes WHERE shape LIKE 'order%' ORDER BY id;

-- promoted into a segment and merged with the others
SELECT sift.col_promote('nn_col') >= 0 AS promoted;
SELECT sift.col_merge('nn_col', true) >= 0 AS merged;
SELECT shape, pushed(q), same_rows(q) AS same FROM shapes WHERE shape NOT LIKE 'order%' ORDER BY id;
SELECT shape, pushed(q), same_ordered(q) AS same FROM shapes WHERE shape LIKE 'order%' ORDER BY id;

-- rows deleted: their NaN is not counted
DELETE FROM nn WHERE g = 3 AND id % 2 = 0;
SELECT shape, pushed(q), same_rows(q) AS same FROM shapes WHERE shape NOT LIKE 'order%' ORDER BY id;
SELECT shape, pushed(q), same_ordered(q) AS same FROM shapes WHERE shape LIKE 'order%' ORDER BY id;

DROP TABLE shapes;
DROP FUNCTION pushed(text), same_rows(text), same_ordered(text), error_of(text, boolean);
DROP TABLE nn;
DROP EXTENSION pg_sift;
