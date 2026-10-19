-- Filter-mask pushdown: a ranked SiftScan whose child is a bitmap of the other quals,
-- turned into page and offset masks the ranker ANDs into its candidates. Every answer must equal the post-filter plan
-- bit for bit (ctids, scores, order) and the sequential evaluation of the same query, for exact, lossy and combined
-- bitmaps, partial and partitioned tables, and rows in every state (updates, deletes, inserts, vacuumed or not).
CREATE EXTENSION pg_sift;

CREATE FUNCTION sift_plan(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	l text;
	r text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		r := r || l || E'\n';
	END LOOP;
	RETURN rtrim(r, E'\n');
END $$;
CREATE FUNCTION sift_scan_node(q text) RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE
	j jsonb;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) ' || q INTO j;
	RETURN (SELECT n FROM jsonb_path_query(j, 'strict $.**') n WHERE n ->> 'Custom Plan Provider' = 'SiftScan' LIMIT 1);
END $$;

-- 4 superblocks of rows 300 bytes wide: tenant spreads over every page (80 rows of 500 tenants), cat is clustered
-- (BRIN), tags is for GIN
CREATE TABLE fd (id int PRIMARY KEY, tenant int, cat int, tags int[], body text, pad text) WITH (autovacuum_enabled = off, fillfactor = 70);
INSERT INTO fd
SELECT i, (i * 7919) % 500, i / 300, ARRAY[i % 100, i % 13],
	'common w' || i % 40 || ' x' || i % 7 || ' ' || repeat('rare' || i % 5 || ' ', i % 4)
		|| CASE WHEN i % 11 = 0 THEN 'quick brown fox' ELSE 'filler words here' END, repeat('p', 200)
FROM generate_series(1, 24000) i;
CREATE INDEX fd_body ON fd USING sift (body);
CREATE INDEX fd_tenant ON fd (tenant);
CREATE INDEX fd_cat ON fd USING brin (cat) WITH (pages_per_range = 4);
CREATE INDEX fd_tags ON fd USING gin (tags);
VACUUM ANALYZE fd;
SELECT pg_relation_size('fd') / 8192 / 256 AS superblocks;

-- the ranked top k of the query under the filter, ctids and scores, in one plan mode
CREATE FUNCTION pd_top(tbl regclass, q text, flt text, k int, mode text, OUT ids tid[], OUT metrics_a real[]) LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.filter_pushdown', mode, true);
	EXECUTE format('SELECT array_agg(c ORDER BY n), array_agg(s ORDER BY n) FROM (SELECT row_number() OVER () AS n, c, s
		FROM (SELECT ctid AS c, sift.score(ctid) AS s FROM %s WHERE body @@> %L AND (%s) ORDER BY sift.score(ctid) DESC LIMIT %s) x) y',
		tbl, q, flt, k) INTO ids, metrics_a;
	PERFORM set_config('sift.filter_pushdown', 'on', true);
END $$;

-- the plan kind of that query: pushdown (a bitmap under the SiftScan), post-filter or other
CREATE FUNCTION pd_kind(tbl regclass, q text, flt text, mode text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	p text;
BEGIN
	PERFORM set_config('sift.filter_pushdown', mode, true);
	p := sift_plan(format('SELECT sift.score(ctid) FROM %s WHERE body @@> %L AND (%s) ORDER BY sift.score(ctid) DESC LIMIT 10', tbl, q, flt));
	PERFORM set_config('sift.filter_pushdown', 'on', true);
	RETURN CASE WHEN p !~ 'SiftScan' THEN 'other' WHEN p ~ 'Bitmap Index Scan' THEN 'pushdown' ELSE 'post-filter' END;
END $$;

-- the matches of the queries by sequential evaluation, per table state
CREATE TABLE pd_queries (q text PRIMARY KEY);
INSERT INTO pd_queries VALUES ('common'), ('w3 x2'), ('w3 OR x2 OR w9'), ('common NOT w3'), ('"quick brown"'), ('fox^3 OR rare1'),
	('w1*'), ('quik~1 OR rare3'), ('/w[12]/ AND filler'), ('{rare1 rare2 rare3}'), ('nothing');
CREATE TABLE pd_seq (q text, tid tid);
CREATE FUNCTION pd_seq_load(tbl regclass) RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	sq text;
BEGIN
	TRUNCATE pd_seq;
	PERFORM set_config('sift.enable_scan', 'off', true), set_config('enable_bitmapscan', 'off', true),
		set_config('enable_indexscan', 'off', true);
	FOR sq IN SELECT q FROM pd_queries LOOP
		EXECUTE format('INSERT INTO pd_seq SELECT %L, ctid FROM %s WHERE sift.match(body, %L)', sq, tbl, sq);
	END LOOP;
	PERFORM set_config('sift.enable_scan', 'on', true), set_config('enable_bitmapscan', 'on', true),
		set_config('enable_indexscan', 'on', true);
END $$;

-- ok, or what differs: pushdown against the post-filter plan for several k, and for k = everything also
-- against the sequential matches under the filter
CREATE FUNCTION pd_check(tbl regclass, q text, flt text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	k int;
	a record;
	b record;
	seq tid[];
BEGIN
	FOREACH k IN ARRAY ARRAY[1, 7, 25, 1000000] LOOP
		a := pd_top(tbl, q, flt, k, 'force');
		b := pd_top(tbl, q, flt, k, 'off');
		IF a.ids IS DISTINCT FROM b.ids THEN
			RETURN format('k %s: ctids differ: pushdown %s rows, post-filter %s rows', k, cardinality(a.ids), cardinality(b.ids));
		ELSIF (SELECT array_agg(float4send(s)) FROM unnest(a.metrics_a) s) IS DISTINCT FROM (SELECT array_agg(float4send(s)) FROM unnest(b.metrics_a) s) THEN
			RETURN format('k %s: metrics_a differ', k);
		END IF;
	END LOOP;
	EXECUTE format('SELECT array_agg(ctid ORDER BY ctid) FROM %s WHERE ctid IN (SELECT tid FROM pd_seq WHERE q = %L) AND (%s)', tbl, q, flt) INTO seq;
	IF seq IS DISTINCT FROM (SELECT array_agg(t ORDER BY t) FROM unnest(a.ids) t) THEN
		RETURN format('matches differ from the sequential evaluation: %s against %s rows', cardinality(a.ids), cardinality(seq));
	END IF;
	RETURN 'ok';
END $$;

CREATE TABLE pd_filters (flt text PRIMARY KEY, what text);
INSERT INTO pd_filters VALUES
	('tenant = 7', 'btree equality'), ('tenant < 12', 'btree range'), ('tenant IN (3, 4, 5)', 'btree ScalarArrayOp'),
	('cat = 17', 'BRIN, lossy by nature'), ('cat BETWEEN 40 AND 42', 'BRIN range'), ('tags && ARRAY[5]', 'GIN, recheck'),
	('tenant = 7 AND cat = 17', 'BitmapAnd of btree and BRIN'), ('tenant = 7 OR tenant = 9', 'BitmapOr'),
	('tenant = 7 AND id % 2 = 0', 'btree and a qual no index serves'), ('tenant = 999', 'nothing'),
	('tenant < 300', 'half the table'), ('id % 2 = 0', 'no index serves it');

-- the filters named in whats, else all of them (a WHERE on the result would run every check first: pd_check is volatile)
CREATE FUNCTION pd_run(tbl regclass, whats text[] DEFAULT NULL) RETURNS TABLE (what text, plan text, checked int, failures text) LANGUAGE sql AS $$
	SELECT f.what, pd_kind(tbl, 'w3 OR x2', f.flt, 'force'), count(*)::int, coalesce(string_agg(q.q || ': ' || r, '; ') FILTER (WHERE r <> 'ok'), 'none')
	FROM pd_filters f CROSS JOIN pd_queries q, LATERAL pd_check(tbl, q.q, f.flt) r
	WHERE whats IS NULL OR f.what = ANY (whats)
	GROUP BY f.flt, f.what ORDER BY f.flt
$$;

SELECT pd_seq_load('fd');
SELECT * FROM pd_run('fd');

-- what the filters keep
SELECT flt, (SELECT count(*) FROM fd WHERE ctid IN (SELECT tid FROM pd_seq WHERE q = 'common') AND (CASE flt
	WHEN 'tenant = 7' THEN tenant = 7 WHEN 'cat = 17' THEN cat = 17 WHEN 'tenant < 300' THEN tenant < 300 END)) AS matches
FROM pd_filters WHERE flt IN ('tenant = 7', 'cat = 17', 'tenant < 300') ORDER BY 1;

-- a bitmap over the limit of work_mem has lossy pages: the masks keep their pages whole and the quals still decide
SET work_mem = '64kB';
SET sift.filter_pushdown = force;
SELECT (j ->> 'Filter Pages Lossy')::int > 0 AS lossy
FROM (SELECT sift_scan_node('SELECT id FROM fd WHERE body @@> ''common'' AND tenant < 300 ORDER BY sift.score(ctid) DESC LIMIT 5') AS j) t;
RESET sift.filter_pushdown;
SELECT * FROM pd_run('fd', ARRAY['half the table', 'btree range', 'BitmapOr']);
RESET work_mem;

-- planning: a selective filter pushes down, a filter that keeps half the table does not, off and force say so
SELECT pd_kind('fd', 'common', 'tenant = 7', 'on') AS selective, pd_kind('fd', 'common', 'tenant < 300', 'on') AS half,
	pd_kind('fd', 'common', 'tenant = 7', 'off') AS off, pd_kind('fd', 'common', 'tenant < 300', 'force') AS forced,
	pd_kind('fd', 'common', 'id % 2 = 0', 'force') AS no_index;
EXPLAIN (COSTS OFF) SELECT id, sift.score(ctid) FROM fd WHERE body @@> 'common' AND tenant = 7 ORDER BY sift.score(ctid) DESC LIMIT 5;
EXPLAIN (COSTS OFF) SELECT id, sift.score(ctid) FROM fd WHERE body @@> 'common' AND tenant = 7 AND cat = 17 ORDER BY sift.score(ctid) DESC LIMIT 5;
SET sift.filter_pushdown = off;
EXPLAIN (COSTS OFF) SELECT id, sift.score(ctid) FROM fd WHERE body @@> 'common' AND tenant = 7 ORDER BY sift.score(ctid) DESC LIMIT 5;
RESET sift.filter_pushdown;
SET enable_bitmapscan = off;
EXPLAIN (COSTS OFF) SELECT id, sift.score(ctid) FROM fd WHERE body @@> 'common' AND tenant = 7 ORDER BY sift.score(ctid) DESC LIMIT 5;
RESET enable_bitmapscan;

-- the counters: the mask leaves the ranker the superblocks and documents the filter keeps
SELECT j -> 'Filter Superblocks' AS filter_sbs, (j ->> 'Candidates')::int < 30 AS few_candidates
FROM (SELECT sift_scan_node('SELECT id FROM fd WHERE body @@> ''common'' AND tenant = 7 ORDER BY sift.score(ctid) DESC LIMIT 5') AS j) t;
SET sift.filter_pushdown = off;
SELECT (j ->> 'Candidates')::int > 1000 AS many_candidates, j ? 'Filter Superblocks' AS has_filter
FROM (SELECT sift_scan_node('SELECT id FROM fd WHERE body @@> ''common'' AND tenant = 7 ORDER BY sift.score(ctid) DESC LIMIT 5') AS j) t;
RESET sift.filter_pushdown;

-- parameters, rescans, a NULL operand and an empty bitmap
PREPARE pt(int, text, int) AS SELECT id, sift.score(ctid) FROM fd WHERE body @@> $2 AND tenant = $1 ORDER BY sift.score(ctid) DESC LIMIT $3;
SET sift.filter_pushdown = force;
EXECUTE pt(7, 'w3 OR x2 OR fox', 3);
EXECUTE pt(8, 'w3 OR x2 OR fox', 3);
EXECUTE pt(999, 'w3 OR x2 OR fox', 3);
EXECUTE pt(7, NULL, 3);
EXECUTE pt(NULL, 'w3 OR x2 OR fox', 3);
SELECT t.tenant, s.id FROM (VALUES (7), (8), (999), (7)) t(tenant),
	LATERAL (SELECT id FROM fd WHERE body @@> 'w3 OR x2' AND tenant = t.tenant ORDER BY sift.score(ctid) DESC LIMIT 2) s ORDER BY 1, 2;
SELECT (SELECT array_agg(id) FROM (SELECT id FROM fd WHERE body @@> 'w3 OR x2' AND tenant = t.tenant ORDER BY sift.score(ctid) DESC LIMIT 2) s)
	IS NOT DISTINCT FROM (SELECT array_agg(id) FROM (SELECT id FROM fd WHERE body @@> 'w3 OR x2' AND tenant = t.tenant ORDER BY sift.score(ctid) DESC LIMIT 2) s)
FROM (VALUES (7), (8)) t(tenant);
-- a plan cached with a bitmap runs again after the filter's index is gone and replans
EXECUTE pt(7, 'w3 OR x2 OR fox', 3);
DROP INDEX fd_tenant;
EXECUTE pt(7, 'w3 OR x2 OR fox', 3);
CREATE INDEX fd_tenant ON fd (tenant);
EXECUTE pt(7, 'w3 OR x2 OR fox', 3);
RESET sift.filter_pushdown;
DEALLOCATE pt;
ANALYZE fd;

-- a score above the scan, OFFSET past the limit hint, and the score of other columns
SET sift.filter_pushdown = force;
SELECT id, sift.score(ctid) = sift.score(ctid) AS stable FROM fd WHERE body @@> 'w3 OR x2' AND tenant = 7 ORDER BY sift.score(ctid) DESC, id LIMIT 3;
SELECT count(*) FROM (SELECT id FROM fd WHERE body @@> 'common' AND tenant = 7 ORDER BY sift.score(ctid) DESC OFFSET 0) t;
SELECT id FROM fd WHERE body @@> 'w3 OR x2' AND tenant = 7 ORDER BY sift.score(ctid) DESC, id LIMIT 3 OFFSET 2;
RESET sift.filter_pushdown;

-- rows in every state: HOT and other updates, deletes, inserts (write buffers), before and after VACUUM,
-- promotion and merge
UPDATE fd SET pad = pad || 'z' WHERE id % 50 = 0;
UPDATE fd SET tenant = (tenant + 1) % 500 WHERE id % 97 = 0;
UPDATE fd SET body = body || ' extra w3' WHERE id % 89 = 0;
DELETE FROM fd WHERE id % 53 = 0;
INSERT INTO fd SELECT i, (i * 7919) % 500, i / 300, ARRAY[i % 100, i % 13], 'common w' || i % 40 || ' x' || i % 7 || ' fox', 'p'
FROM generate_series(24001, 25000) i;
SELECT pd_seq_load('fd');
SELECT * FROM pd_run('fd');
-- a transaction sees its own rows, and not the ones it rolled back
BEGIN;
INSERT INTO fd SELECT i, 7, 17, ARRAY[5], 'common w3 x2 fox', 'p' FROM generate_series(30001, 30050) i;
DELETE FROM fd WHERE tenant = 7 AND id < 5000;
SELECT pd_seq_load('fd');
SELECT * FROM pd_run('fd', ARRAY['btree equality', 'BitmapAnd of btree and BRIN']);
ROLLBACK;
SELECT pd_seq_load('fd');
-- after VACUUM and after the merge one filter of each bitmap kind: the index kinds do not interact with the state of the sift index
VACUUM fd;
SELECT * FROM pd_run('fd', ARRAY['btree equality', 'btree range', 'BRIN, lossy by nature', 'GIN, recheck', 'BitmapAnd of btree and BRIN', 'BitmapOr', 'half the table']);
SELECT sift.promote('fd_body') IS NOT NULL AS promoted;
SELECT * FROM pd_run('fd', ARRAY['btree equality', 'BRIN, lossy by nature', 'BitmapOr']);
INSERT INTO fd SELECT i, (i * 7919) % 500, i / 300, ARRAY[i % 100], 'common w3 x2 late', 'p' FROM generate_series(25001, 25600) i;
SELECT pd_seq_load('fd');
SELECT * FROM pd_run('fd', ARRAY['btree equality', 'BRIN, lossy by nature', 'BitmapOr']);
SELECT sift.merge('fd_body', true) IS NOT NULL AS merged;
SELECT * FROM pd_run('fd', ARRAY['btree equality', 'btree range', 'BRIN, lossy by nature', 'GIN, recheck', 'BitmapAnd of btree and BRIN', 'BitmapOr', 'half the table']);
SELECT sift.verify('fd_body', true);

-- a partial sift index, only used under its predicate
CREATE TABLE fp (id int, tenant int, live bool, body text);
INSERT INTO fp SELECT i, i % 40, i % 3 <> 0, 'common w' || i % 9 || ' x' || i % 4 FROM generate_series(1, 6000) i;
CREATE INDEX fp_body ON fp USING sift (body) WHERE live;
CREATE INDEX fp_tenant ON fp (tenant);
ANALYZE fp;
SELECT pd_seq_load('fp');
SELECT pd_check('fp', q, 'live AND tenant = 7') AS partial_live FROM pd_queries q ORDER BY q.q LIMIT 4;
SELECT pd_kind('fp', 'common', 'live AND tenant = 7', 'force') AS partial_kind, pd_kind('fp', 'common', 'tenant = 7', 'force') AS predicate_missing;
DROP TABLE fp;

-- partitions: each one plans its own bitmap, also in a ranked scan through the partitioned parent (rank_partition)
CREATE TABLE fq (id int, tenant int, body text) PARTITION BY RANGE (id);
CREATE TABLE fq1 PARTITION OF fq FOR VALUES FROM (0) TO (8000);
CREATE TABLE fq2 PARTITION OF fq FOR VALUES FROM (8000) TO (16000);
INSERT INTO fq SELECT i, (i * 7919) % 500, 'common w' || i % 40 || ' x' || i % 7 || repeat(' pad', i % 5) FROM generate_series(1, 15999) i;
CREATE INDEX fq_body ON fq USING sift (body);
CREATE INDEX fq_tenant ON fq (tenant);
ANALYZE fq;
SELECT pd_seq_load('fq1');
SELECT pd_check('fq1', q, 'tenant = 7') AS partition_1 FROM pd_queries q ORDER BY q.q LIMIT 4;
SELECT pd_kind('fq2', 'w3 OR x2', 'tenant = 7', 'on') AS partition_2;
SET sift.filter_pushdown = force;
SELECT count(*) FROM fq WHERE body @@> 'w3 OR x2' AND tenant = 7;
SET sift.filter_pushdown = off;
SELECT count(*) FROM fq WHERE body @@> 'w3 OR x2' AND tenant = 7;
RESET sift.filter_pushdown;
DROP TABLE fq;

DROP TABLE fd, pd_queries, pd_seq, pd_filters;
DROP FUNCTION pd_top(regclass, text, text, int, text), pd_kind(regclass, text, text, text), pd_seq_load(regclass),
	pd_check(regclass, text, text), pd_run(regclass, text[]), sift_plan(text), sift_scan_node(text);
DROP EXTENSION pg_sift;
