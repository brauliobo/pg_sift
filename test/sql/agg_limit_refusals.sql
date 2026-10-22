-- A query past a fixed limit of the extension (docs/LIMITS.md) keeps the normal plan with a slug that names the limit and its value, and sift.stats()
-- counts it twice, as refused.<slug> and as limit.<id>. Each limit is lowered with sift.test_limits so that a few rows reach it; the answer must be
-- the one the scan gives under the default limit (the refusal changes the plan, not the rows).
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET statement_timeout = '30s';
SET enable_seqscan = off;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SET enable_indexonlyscan = off;

CREATE TABLE events (id int PRIMARY KEY, parent_id int, kind text, ts date);
INSERT INTO events SELECT i, i % 600, 'k' || i % 5, date '2025-01-01' + i % 6 FROM generate_series(1, 12000) i;
CREATE INDEX events_col ON events USING sift_col (id, parent_id, kind, ts);
CREATE TABLE dims (id int PRIMARY KEY, label text);
INSERT INTO dims SELECT i, 'd' || i % 7 FROM generate_series(0, 599) i;
CREATE TABLE ticks (a int);
INSERT INTO ticks SELECT i FROM generate_series(0, 599) i;
CREATE TABLE parted (id int, kind text, val int) PARTITION BY HASH (id);
CREATE TABLE parted_0 PARTITION OF parted FOR VALUES WITH (MODULUS 2, REMAINDER 0);
CREATE TABLE parted_1 PARTITION OF parted FOR VALUES WITH (MODULUS 2, REMAINDER 1);
INSERT INTO parted SELECT i, 'k' || (i % 5), i % 100 FROM generate_series(1, 2000) i;
CREATE INDEX parted_0_col ON parted_0 USING sift_col (id, kind, val);
CREATE INDEX parted_1_col ON parted_1 USING sift_col (id, kind, val);
VACUUM (FREEZE, ANALYZE) events;
ANALYZE dims; ANALYZE ticks; ANALYZE parted;

-- the arms of a two level aggregate, joined to a spine of ticks
CREATE FUNCTION spine_q() RETURNS text LANGUAGE sql AS $$
	SELECT $q$SELECT m.kind, sp.k, sum(m.n) FROM (SELECT a FROM ticks) sp(k) JOIN
	          (SELECT id, parent_id, kind, count(*) AS n FROM events GROUP BY id, parent_id, kind
	           UNION ALL SELECT id, parent_id, kind, count(*) FROM events GROUP BY id, parent_id, kind) m ON m.parent_id = sp.k GROUP BY 1, 2$q$
$$;

CREATE TABLE limits (id text, setting text, arm text, slug text, q text);
INSERT INTO limits VALUES
 ('group_keys', 'group_keys=2', 'single', 'too_many_keys', 'SELECT count(*) FROM events GROUP BY kind, ts, parent_id'),
 ('joins', 'joins=0', 'single', 'shape_width', 'SELECT d.label, count(*) FROM events e JOIN dims d ON d.id = e.parent_id GROUP BY 1'),
 ('combos', 'combos=0', 'single', 'shape_width', 'SELECT d.label, count(*) FROM events e JOIN dims d ON d.id = e.parent_id GROUP BY 1'),
 ('partitions', 'partitions=1', 'single', 'partition_count', 'SELECT kind, count(*) FROM parted GROUP BY 1');

-- the slug, and the detail that names the limit and the value it was lowered to
CREATE FUNCTION why(q text, setting text, a text) RETURNS TABLE (used bool, slug text, reason text) LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.test_limits', setting, true);
	RETURN QUERY SELECT w.used, w.slug, w.reason FROM sift.explain_why(q) w WHERE w.arm = a;
END $$;
SELECT l.id, w.used, w.slug = l.slug AS slug_ok, w.reason ~ ('the limit ' || l.id || ' is ' || split_part(l.setting, '=', 2) || '\)$') AS names_limit
FROM limits l, LATERAL why(l.q, l.setting, l.arm) w ORDER BY l.ctid;

-- the budget of a lookup table is a setting, not a hooked limit: the spine of ticks is refused under 1 kB and the detail names lookup_kb
SET sift.agg_lookup_max_kb = 1;
SELECT w.used, w.slug, w.reason ~ 'rows of width \d+, the limit lookup_kb is 1\)$' AS names_limit FROM sift.explain_why(spine_q()) w WHERE w.arm = 'two_level';
RESET sift.agg_lookup_max_kb;

-- the counters of the session, per slug and per limit
SELECT sift.stats_reset();
SET sift.count_refusals = on;
SET client_min_messages = warning;
DO $$
DECLARE r record;
BEGIN
	FOR r IN SELECT * FROM limits ORDER BY ctid LOOP
		PERFORM set_config('sift.test_limits', r.setting, false);
		EXECUTE 'EXPLAIN (COSTS OFF) ' || r.q;
	END LOOP;
	PERFORM set_config('sift.test_limits', '', false);
	SET sift.agg_lookup_max_kb = 1;
	EXECUTE 'EXPLAIN (COSTS OFF) ' || spine_q();
	RESET sift.agg_lookup_max_kb;
END $$;
RESET sift.count_refusals;
SELECT name, value > 0 AS counted FROM sift.stats() WHERE name LIKE 'limit.%' OR name IN ('refused.too_many_keys', 'refused.shape_width', 'refused.spine_rows') ORDER BY 1;
SELECT count(*) = 0 AS only_limits FROM sift.stats() s WHERE s.name LIKE 'limit.%' AND substr(s.name, 7) NOT IN (SELECT id FROM limits UNION SELECT 'lookup_kb');

-- same rows under the default limit and under the lowered one
CREATE FUNCTION answer(q text, setting text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.test_limits', setting, true);
	EXECUTE format('SELECT md5(string_agg(t::text, E''\n'' ORDER BY t::text)) FROM (%s) t', q) INTO r;
	RETURN r;
END $$;
SELECT l.id, answer(l.q, '') = answer(l.q, l.setting) AS same_rows FROM limits l ORDER BY l.ctid;
SET sift.agg_lookup_max_kb = 1;
SELECT answer(spine_q(), '') AS small_budget \gset
RESET sift.agg_lookup_max_kb;
SELECT answer(spine_q(), '') = :'small_budget' AS spine_same_rows;

-- the registry names the slug for what the limit does
SELECT slug, class FROM sift.refusal_slugs() WHERE slug IN ('shape_width', 'too_many_keys', 'spine_rows', 'where') ORDER BY slug;

RESET client_min_messages;
DROP TABLE limits, events, dims, ticks, parted;
DROP FUNCTION spine_q(), answer(text, text), why(text, text, text);
DROP EXTENSION pg_sift;
