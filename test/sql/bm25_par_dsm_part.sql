-- A pushed down filter is built once and shared by the participants of a parallel ranked scan: the first participant that
-- needs it runs the bitmap and publishes its masks in the DSM area, the others read them in place. Whatever the filter keeps,
-- the parallel scan returns what the serial one returns: rows, scores and ties.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;

CREATE TABLE pd (id int PRIMARY KEY, tenant int, body text, pad text) WITH (autovacuum_enabled = off, fillfactor = 70, parallel_workers = 3);
ALTER TABLE pd ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO pd SELECT i, (i * 7919) % 500, 'common w' || i % 40 || ' x' || i % 7 || repeat(' filler', i % 4)
	|| CASE WHEN i % 11 = 0 THEN ' quick brown fox' ELSE '' END, repeat('x', 1500) FROM generate_series(1, 7000) i;
CREATE INDEX pd_body ON pd USING sift (body) WITH (write_buffer_limit = 64);
CREATE INDEX pd_tenant ON pd (tenant);
DELETE FROM pd WHERE id % 10 = 3;
VACUUM ANALYZE pd;
INSERT INTO pd SELECT i, (i * 7919) % 500, 'common w' || i % 40 || ' late', repeat('x', 1500) FROM generate_series(7001, 7500) i;
SELECT pg_relation_size('pd') / current_setting('block_size')::int / 256 >= 3 AS superblocks;

CREATE FUNCTION pd_set(workers int) RETURNS void LANGUAGE sql AS $$
	SELECT set_config('max_parallel_workers_per_gather', workers::text, true),
		set_config('parallel_setup_cost', '0', true), set_config('parallel_tuple_cost', '0', true),
		set_config('min_parallel_index_scan_size', '0', true), set_config('sift.filter_pushdown', 'force', true)
$$;

CREATE FUNCTION pd_plan(q text, flt text, k int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	p text;
BEGIN
	PERFORM pd_set(3);
	p := sift_helpers.plan_text(format('SELECT id, sift.score(ctid) FROM pd WHERE body @@> %L AND %s ORDER BY sift.score(ctid) DESC LIMIT %s', q, flt, k));
	RETURN CASE WHEN p ~ 'Parallel Custom Scan \(SiftScan\)' AND p ~ 'Bitmap Index Scan' THEN 'parallel pushdown'
	            WHEN p ~ 'SiftScan' AND p ~ 'Bitmap Index Scan' THEN 'serial pushdown' ELSE 'other' END;
END $$;

CREATE FUNCTION pd_top(q text, flt text, k int, workers int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	r text;
BEGIN
	PERFORM pd_set(workers);
	EXECUTE format('SELECT md5(coalesce(string_agg(id || '':'' || s || '':'' || c, '','' ORDER BY n), '''')) FROM (SELECT row_number() OVER () AS n, id, s, c
		FROM (SELECT id, sift.score(ctid)::text AS s, ctid::text AS c FROM pd WHERE body @@> %L AND %s ORDER BY sift.score(ctid) DESC LIMIT %s) x) y',
		q, flt, k) INTO r;
	RETURN r;
END $$;

CREATE FUNCTION pd_rows(q text, flt text, k int, workers int) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE
	n bigint;
BEGIN
	PERFORM pd_set(workers);
	EXECUTE format('SELECT count(*) FROM (SELECT id FROM pd WHERE body @@> %L AND %s ORDER BY sift.score(ctid) DESC LIMIT %s) x', q, flt, k) INTO n;
	RETURN n;
END $$;

-- the plan: one SiftScan under the Gather, the bitmap of the filter its child
SELECT pd_plan('common w3', 'tenant < 50', 10);
SELECT pd_plan('common w3', 'tenant < 250', 1000);

-- the same rows, scores and ties as the serial scan, for filters that keep a few rows, many, all, none
SELECT q, flt, k, pd_rows(q, flt, k, 3) AS n,
	pd_top(q, flt, k, 3) = pd_top(q, flt, k, 0) AS same_as_serial
FROM (VALUES ('common w3', 'tenant < 50'), ('common w3', 'tenant = 7'), ('common', 'tenant < 250'), ('common x2', 'tenant >= 0'),
	('common w3 OR x2', 'tenant BETWEEN 10 AND 30'), ('"quick brown"', 'tenant < 400'), ('common', 'tenant < 0'), ('moved', 'tenant < 100'),
	('late', 'tenant < 300')) v(q, flt),
	unnest(ARRAY[1, 10, 100, 100000]) k
ORDER BY q, flt, k;

-- a rescanned Gather publishes the filter again: the scan is the inner side of a nested loop
CREATE FUNCTION pd_rescan(workers int, OUT plan text, OUT answer text) LANGUAGE plpgsql AS $$
DECLARE
	q text := 'SELECT * FROM (SELECT id, sift.score(ctid)::text AS s FROM pd WHERE body @@> ''common w3'' AND tenant < 60
		ORDER BY sift.score(ctid) DESC LIMIT 5) t RIGHT JOIN (VALUES (1), (2), (3)) v(x) ON true';
BEGIN
	PERFORM pd_set(workers);
	PERFORM set_config('enable_material', 'off', true);
	plan := CASE WHEN sift_helpers.plan_text(q) ~ 'Gather' AND sift_helpers.plan_text(q) ~ 'Bitmap Index Scan on pd_tenant' THEN 'parallel pushdown' ELSE 'serial' END;
	EXECUTE 'SELECT md5(string_agg(x || '':'' || id || '':'' || s, '','' ORDER BY x, id)) FROM (' || q || ') j' INTO answer;
END $$;
SELECT (pd_rescan(3)).plan;
SELECT (pd_rescan(3)).answer = (pd_rescan(0)).answer AS rescan_same_as_serial;

DROP TABLE pd;
DROP FUNCTION pd_set(int), pd_plan(text, text, int), pd_top(text, text, int, int), pd_rows(text, text, int, int), pd_rescan(int);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
