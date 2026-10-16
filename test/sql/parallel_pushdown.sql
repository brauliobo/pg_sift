-- Filter pushdown (sift.filter_pushdown) and parallel scans (sift.parallel_scan) together: the bitmap of a pushed-down
-- filter is built by one backend, so a ranked scan with a bitmap child is never parallel-aware; the parallel path is
-- planned without a filter and competes with it on cost. Every combination of the two GUCs answers what both off answer.
CREATE EXTENSION pg_sift;

CREATE FUNCTION pp_plan(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	l text;
	r text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		r := r || l || E'\n';
	END LOOP;
	RETURN rtrim(r, E'\n');
END $$;
-- the SiftScan with a bitmap child, a Gather over a SiftScan, a serial SiftScan or another plan
CREATE FUNCTION pp_kind(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	p text := pp_plan(q);
BEGIN
	RETURN CASE WHEN p !~ 'SiftScan' THEN 'other' WHEN p ~ 'Bitmap Index Scan' THEN 'pushdown' WHEN p ~ 'Gather' THEN 'parallel' ELSE 'serial' END;
END $$;

-- tenant spreads over every page; three superblocks to hand out
CREATE TABLE pp (id int PRIMARY KEY, tenant int, body text, pad text) WITH (autovacuum_enabled = off, fillfactor = 70, parallel_workers = 3);
ALTER TABLE pp ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO pp SELECT i, (i * 7919) % 500, 'common w' || i % 40 || ' x' || i % 7 || repeat(' filler', i % 4)
	|| CASE WHEN i % 11 = 0 THEN ' quick brown fox' ELSE '' END, repeat('x', 1500) FROM generate_series(1, 6000) i;
CREATE INDEX pp_body ON pp USING sift (body) WITH (mutable_limit = 64);
CREATE INDEX pp_tenant ON pp (tenant);
INSERT INTO pp SELECT i, (i * 7919) % 500, 'common w' || i % 40 || ' x' || i % 7 || ' late', repeat('x', 1500) FROM generate_series(6001, 7000) i;
DELETE FROM pp WHERE id % 10 = 3;
VACUUM ANALYZE pp;
UPDATE pp SET body = body || ' moved' WHERE id % 19 = 0;
SELECT pg_relation_size('pp') / current_setting('block_size')::int / 256 >= 3 AS superblocks;

SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_index_scan_size = 0;
SET max_parallel_workers_per_gather = 3;

-- the combinations of the GUCs, local to the statement
CREATE FUNCTION pp_set(par text, push text) RETURNS text LANGUAGE sql AS $$
	SELECT set_config('sift.parallel_scan', par, true) || set_config('sift.filter_pushdown', push, true)
$$;
CREATE TABLE pp_modes (par text, push text);
INSERT INTO pp_modes VALUES ('off', 'off'), ('off', 'on'), ('off', 'force'), ('on', 'off'), ('on', 'on'), ('on', 'force');

-- the plan kind where the cost model leaves no choice: a SiftScan with a bitmap child is serial, also with both on, and
-- the parallel path appears only without the pushdown
CREATE FUNCTION pp_kinds(par text, push text, OUT ranked_filtered text, OUT ranked text) LANGUAGE plpgsql AS $$
BEGIN
	PERFORM pp_set(par, push);
	ranked_filtered := pp_kind('SELECT sift.score(ctid) FROM pp WHERE body @@> ''common w3'' AND tenant = 7 ORDER BY sift.score(ctid) DESC LIMIT 10');
	ranked := pp_kind('SELECT sift.score(ctid) FROM pp WHERE body @@> ''common w3'' ORDER BY sift.score(ctid) DESC LIMIT 10');
END $$;
SELECT par, push, (pp_kinds(par, push)).* FROM pp_modes WHERE (par, push) IN (('off', 'off'), ('off', 'force'), ('on', 'off'), ('on', 'force'))
ORDER BY par DESC, push DESC;

-- the ranked top k under the filter, ctids and scores in delivery order, in one combination
CREATE FUNCTION pp_top(q text, flt text, k int, par text, push text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	r text;
BEGIN
	PERFORM pp_set(par, push);
	EXECUTE format('SELECT md5(coalesce(string_agg(c || '':'' || s, '','' ORDER BY n), '''')) FROM (SELECT row_number() OVER () AS n, c, s
		FROM (SELECT ctid::text AS c, sift.score(ctid)::text AS s FROM pp WHERE body @@> %L AND (%s) ORDER BY sift.score(ctid) DESC LIMIT %s) x) y',
		q, flt, k) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION pp_ids(q text, flt text, par text, push text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	r text;
BEGIN
	PERFORM pp_set(par, push);
	EXECUTE format('SELECT md5(coalesce(string_agg(id::text, '','' ORDER BY id), '''')) FROM pp WHERE body @@> %L AND (%s)', q, flt) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION pp_count(q text, flt text, par text, push text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE
	n bigint;
BEGIN
	PERFORM pp_set(par, push);
	EXECUTE format('SELECT count(*) FROM pp WHERE body @@> %L AND (%s)', q, flt) INTO n;
	RETURN n;
END $$;

-- ok, or the combinations whose answer differs from both off
CREATE FUNCTION pp_check(q text, flt text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	m record;
	k int;
	bad text := '';
BEGIN
	FOR m IN SELECT * FROM pp_modes WHERE (par, push) <> ('off', 'off') LOOP
		FOREACH k IN ARRAY ARRAY[1, 10, 100, 100000] LOOP
			IF pp_top(q, flt, k, m.par, m.push) IS DISTINCT FROM pp_top(q, flt, k, 'off', 'off') THEN
				bad := bad || format(' top%s(%s,%s)', k, m.par, m.push);
			END IF;
		END LOOP;
		IF pp_ids(q, flt, m.par, m.push) IS DISTINCT FROM pp_ids(q, flt, 'off', 'off') THEN
			bad := bad || format(' ids(%s,%s)', m.par, m.push);
		END IF;
		IF pp_count(q, flt, m.par, m.push) IS DISTINCT FROM pp_count(q, flt, 'off', 'off') THEN
			bad := bad || format(' count(%s,%s)', m.par, m.push);
		END IF;
	END LOOP;
	RETURN CASE WHEN bad = '' THEN 'ok' ELSE bad END;
END $$;

SELECT q, flt, pp_check(q, flt) AS result
FROM (VALUES ('common', 'tenant = 7'), ('common w3', 'tenant = 7'), ('common w3 OR x2', 'tenant < 3'), ('"quick brown"', 'tenant = 11'),
	('common NOT w3', 'tenant BETWEEN 10 AND 12'), ('w1*', 'tenant = 7 OR tenant = 8'), ('common', 'tenant < 250'),
	('common w3', 'id % 3 = 0 AND tenant < 100'), ('moved', 'tenant < 400'), ('nothing', 'tenant = 7')) v(q, flt);

-- both on: whichever path the costs choose, a bitmap child never sits under a Gather, and a parallel scan has no child
CREATE FUNCTION pp_shape_ok(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE
	p text;
BEGIN
	PERFORM pp_set('on', 'on');
	p := pp_plan(q);
	RETURN NOT (p ~ 'Gather' AND p ~ 'SiftScan' AND p ~ 'Bitmap Index Scan on pp_tenant');
END $$;
SELECT pp_shape_ok(format('SELECT sift.score(ctid) FROM pp WHERE body @@> ''common w3'' AND tenant < %s ORDER BY sift.score(ctid) DESC LIMIT %s', t, k)) AS shape_ok,
	count(*)
FROM unnest(ARRAY[1, 7, 50, 250, 500]) t, unnest(ARRAY[1, 10, 1000]) k GROUP BY 1;

DROP TABLE pp;
DROP EXTENSION pg_sift;
