-- A sum of a numeric column times a value the statement asks once (a sub-select of one row, a parameter) is the sum of the column times that value: the scan computes the sum, the
-- product is made above it. The rows must be those of the scan off, with no worker and with three, for a factor that is NULL, that has more digits than the column, a column with NaN, groups
-- without a value, a HAVING and a filter. An average, a product of two columns and a factor that changes by row stay as they are.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE kinds (id int PRIMARY KEY, w numeric(8,3), n int);
INSERT INTO kinds SELECT g, g / 8.0, CASE WHEN g % 4 = 0 THEN NULL ELSE g END FROM generate_series(1, 30) g;
CREATE TABLE events (id int PRIMARY KEY, tenant_id int NOT NULL, status int NOT NULL, value_num numeric(10,2), loose numeric, qty int) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT g, 1 + (g * 7919) % 400, 1 + g % 5, CASE WHEN g % 13 = 0 THEN NULL ELSE (g % 500) / 5.0 END,
       CASE WHEN g % 997 = 0 THEN 'NaN'::numeric WHEN g % 7 = 0 THEN NULL ELSE (g % 300) / 7.0 END, g % 9 FROM generate_series(1, 60000) g;
-- the groups of status 5 have no value at all
UPDATE events SET value_num = NULL WHERE status = 5;
CREATE INDEX events_col ON events USING sift_col (id, tenant_id, status, value_num, loose, qty) WITH (key_cut = tenant_id);
VACUUM (FREEZE, ANALYZE) events, kinds;

CREATE TABLE factor_q (n int, label text, q text);
INSERT INTO factor_q VALUES
	(1, 'count of a table',                   'SELECT status, sum(value_num * (SELECT count(*) FROM kinds)) AS s FROM events GROUP BY 1'),
	(2, 'factor on the left',                 'SELECT status, sum((SELECT count(*) FROM kinds) * value_num) AS s FROM events GROUP BY 1'),
	(3, 'more digits than the column',        'SELECT status, sum(value_num * (SELECT sum(w) FROM kinds)) AS s FROM events GROUP BY 1'),
	(4, 'a factor that is NULL',              'SELECT status, sum(value_num * (SELECT max(n) FROM kinds WHERE id > 100)) AS s FROM events GROUP BY 1'),
	(5, 'a factor of an expression',          'SELECT status, sum(value_num * ((SELECT count(*) FROM kinds) + 0.5)) AS s FROM events GROUP BY 1'),
	(6, 'a column with NaN',                  'SELECT status, sum(loose * (SELECT count(*) FROM kinds)) AS s FROM events GROUP BY 1'),
	(7, 'two sums and a count',               'SELECT status, sum(value_num * (SELECT count(*) FROM kinds)) AS s, sum(value_num) AS t, count(*) AS c FROM events GROUP BY 1'),
	(8, 'a having',                           'SELECT status, sum(value_num * (SELECT count(*) FROM kinds)) AS s FROM events GROUP BY 1 HAVING sum(value_num * (SELECT count(*) FROM kinds)) > 1000'),
	(9, 'a filter',                           'SELECT status, sum(value_num * (SELECT count(*) FROM kinds)) FILTER (WHERE qty > 3) AS s FROM events GROUP BY 1'),
	(10, 'no group',                          'SELECT sum(value_num * (SELECT count(*) FROM kinds)) AS s FROM events'),
	(11, 'no row',                            'SELECT sum(value_num * (SELECT count(*) FROM kinds)) AS s FROM events WHERE id < 0'),
	(12, 'an average',                        'SELECT status, avg(value_num * (SELECT count(*) FROM kinds)) AS s FROM events GROUP BY 1'),
	(13, 'a product of two columns',          'SELECT status, sum(value_num * loose) AS s FROM events GROUP BY 1'),
	(14, 'a factor that changes by row',      'SELECT status, sum(value_num * (SELECT w FROM kinds k WHERE k.id = events.qty + 1)) AS s FROM events GROUP BY 1'),
	(15, 'a distinct sum',                    'SELECT status, sum(DISTINCT value_num * (SELECT count(*) FROM kinds)) AS s FROM events GROUP BY 1');
-- the plan of each statement: the product made above the scan (the scan holds the sum of the column), the product in the scan, or the normal plan
CREATE FUNCTION factor_plan(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; scans int := 0; product int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		scans := scans + (l LIKE '%Custom Scan (SiftAgg)%')::int;
		product := product + (l LIKE '%Pushed Aggregates:%' AND l LIKE '%value_num *%' OR l LIKE '%Pushed Aggregates:%' AND l LIKE '%* events.value_num%')::int;
	END LOOP;
	RETURN CASE WHEN scans = 0 THEN 'normal' WHEN product > 0 THEN 'scan, product in it' ELSE 'scan' END;
END $$;
CREATE FUNCTION factor_rows(q text, scan bool, workers int) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.rows_agg_with_workers_reset(q, scan, workers) $$;
SELECT n, label, factor_plan(q) AS plan, factor_rows(q, true, 0) = factor_rows(q, false, 0) AS same, factor_rows(q, true, 3) = factor_rows(q, false, 0) AS same_workers FROM factor_q ORDER BY n;

-- a parameter is the same for every row of one execution: the generic plan holds the sum
SET plan_cache_mode = force_generic_plan;
PREPARE by_param(numeric) AS SELECT status, sum(value_num * $1) AS s FROM events GROUP BY 1 ORDER BY 1;
SELECT factor_plan('EXECUTE by_param(2.5)') AS plan;
CREATE TABLE param_rows AS EXECUTE by_param(2.5);
CREATE TABLE param_null AS EXECUTE by_param(NULL);
SET sift.enable_agg = off;
CREATE TABLE param_rows_off AS EXECUTE by_param(2.5);
CREATE TABLE param_null_off AS EXECUTE by_param(NULL);
RESET sift.enable_agg;
SELECT NOT EXISTS (SELECT * FROM param_rows EXCEPT SELECT * FROM param_rows_off) AND NOT EXISTS (SELECT * FROM param_rows_off EXCEPT SELECT * FROM param_rows) AS same_param,
       NOT EXISTS (SELECT * FROM param_null EXCEPT SELECT * FROM param_null_off) AND NOT EXISTS (SELECT * FROM param_null_off EXCEPT SELECT * FROM param_null) AS same_null,
       (SELECT count(*) FROM param_null WHERE s IS NOT NULL) AS not_null_with_null_factor;
DEALLOCATE by_param;
RESET plan_cache_mode;

DROP TABLE param_rows, param_null, param_rows_off, param_null_off;
DROP FUNCTION factor_plan(text);
DROP FUNCTION factor_rows(text, bool, int);
DROP TABLE factor_q, events, kinds;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
