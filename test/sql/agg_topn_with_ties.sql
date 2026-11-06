-- FETCH FIRST n ROWS WITH TIES is a top-N of the scan: it gives the n best rows and every row that ties with the n-th on the sort keys (the heap is given room for the rows
-- the statistics say tie, and starts again 16 times larger, twice, when the last row of it still ties; past the cap of the heap every row is sorted). The rows are the normal
-- plan's: a hot value that outgrows the room (case 1b), NULLs that tie, two keys, an OFFSET that ends a group of ties, a filter, a key out of the select list, a parameter, a text key
-- (not a scan key of ties: the normal plan) and a heap cap lower than the ties.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;

-- 25,000 rows over 500 values of k (50 each); the 5,000 rows added after the statistics are 4,000 of one hot value and 1,000 NULL
CREATE TABLE wt (id int, k int, g int, t text) WITH (autovacuum_enabled = false);
INSERT INTO wt SELECT i, i % 500, i % 3, 'v' || lpad((i % 500)::text, 4, '0') FROM generate_series(1, 25000) i;
CREATE INDEX wt_col ON wt USING sift_col (id, k, g, t) WITH (order_columns = 'k', order_array_columns = 'k');
VACUUM ANALYZE wt;
INSERT INTO wt SELECT i, CASE WHEN i % 5 = 0 THEN NULL ELSE 9999 END, i % 3, 'v' || CASE WHEN i % 5 = 0 THEN 'nul' ELSE '9999' END FROM generate_series(25001, 30000) i;
SELECT sift.col_promote('wt_col');

CREATE FUNCTION wt_rows(q text, push bool) RETURNS jsonb[] LANGUAGE plpgsql AS $$
DECLARE r jsonb[];
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	EXECUTE format('SELECT array_agg(to_jsonb(q) ORDER BY to_jsonb(q)::text) FROM (%s) q', q) INTO r;
	RETURN r;
END $$;

CREATE FUNCTION wt_check(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a jsonb[]; b jsonb[]; l text; topn bool := false;
BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		topn := topn OR l LIKE '%Top-N:%';
	END LOOP;
	a := wt_rows(q, true);
	b := wt_rows(q, false);
	RETURN CASE WHEN a IS NOT DISTINCT FROM b THEN 'same ' || coalesce(cardinality(a), 0) ELSE 'DIFFERENT' END || CASE WHEN topn THEN ' topn' ELSE ' normal' END;
END $$;

CREATE TABLE wt_q (n text PRIMARY KEY, q text);
INSERT INTO wt_q VALUES
	('1a nulls first',   'SELECT id, k FROM wt ORDER BY k DESC FETCH FIRST 5 ROWS WITH TIES'),
	('1b hot value',     'SELECT id, k FROM wt ORDER BY k DESC NULLS LAST FETCH FIRST 5 ROWS WITH TIES'),
	('2 few ties',       'SELECT id, k FROM wt WHERE id <= 25000 ORDER BY k DESC FETCH FIRST 5 ROWS WITH TIES'),
	('3 ascending',      'SELECT id, k FROM wt ORDER BY k FETCH FIRST 5 ROWS WITH TIES'),
	('4 two keys',       'SELECT id, k, g FROM wt ORDER BY k DESC NULLS LAST, g FETCH FIRST 5 ROWS WITH TIES'),
	('5 offset at group', 'SELECT id, k FROM wt ORDER BY k DESC NULLS LAST OFFSET 4000 ROWS FETCH FIRST 5 ROWS WITH TIES'),
	('6 filter',         'SELECT id, k FROM wt WHERE g = 1 ORDER BY k DESC NULLS LAST FETCH FIRST 3 ROWS WITH TIES'),
	('7 key not read',   'SELECT id FROM wt ORDER BY k DESC NULLS LAST FETCH FIRST 3 ROWS WITH TIES'),
	('8 inside a group', 'SELECT id, k FROM wt ORDER BY k DESC NULLS LAST FETCH FIRST 4500 ROWS WITH TIES'),
	('9 text key',       'SELECT id, t FROM wt ORDER BY t DESC FETCH FIRST 5 ROWS WITH TIES'),
	('10 no row',        'SELECT id, k FROM wt WHERE id < 0 ORDER BY k DESC FETCH FIRST 5 ROWS WITH TIES'),
	('11 count',         'SELECT count(*) FROM (SELECT k FROM wt ORDER BY k DESC NULLS LAST FETCH FIRST 1 ROWS WITH TIES) q');
SELECT n, wt_check(q) FROM wt_q ORDER BY n;

-- a parameter: the generic plan holds the count the scan reads when it starts
SET plan_cache_mode = force_generic_plan;
CREATE FUNCTION wt_par(n bigint, push bool) RETURNS jsonb[] LANGUAGE plpgsql AS $$
DECLARE r jsonb[];
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	SELECT array_agg(to_jsonb(q) ORDER BY to_jsonb(q)::text) INTO r FROM (SELECT id, k FROM wt ORDER BY k DESC NULLS LAST FETCH FIRST n ROWS WITH TIES) q;
	RETURN r;
END $$;
SELECT wt_par(3, true) = wt_par(3, false) AS p3, wt_par(4001, true) = wt_par(4001, false) AS p4001, cardinality(wt_par(3, true)) AS rows3;
DROP FUNCTION wt_par(bigint, bool);
RESET plan_cache_mode;

-- the ties outgrow the heap cap: the rows are sorted and the Limit takes the ties
SET sift.test_limits = 'topn_rows=1000';
SELECT n, wt_check(q) FROM wt_q WHERE n IN ('1b hot value', '4 two keys', '8 inside a group') ORDER BY n;
RESET sift.test_limits;

DROP TABLE wt_q;
DROP FUNCTION wt_check(text), wt_rows(text, bool);
DROP TABLE wt;
DROP EXTENSION pg_sift;
