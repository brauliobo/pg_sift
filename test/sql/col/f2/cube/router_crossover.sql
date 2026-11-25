-- The choice between the summary and the join across the size of the scope: a key of a few members and a panel of thousands, on facts ordered by member first or by time first, read as one key, as an
-- array of keys and with a window that cuts months. Each statement returns the rows of the join as written and of the node when it is chosen.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET TimeZone = 'UTC';
SET sift.test_create_format1 = off;

CREATE TABLE pairs (member int NOT NULL, grp int NOT NULL, PRIMARY KEY (member, grp)) WITH (autovacuum_enabled = false);
INSERT INTO pairs SELECT m, 1 FROM generate_series(1, 5) m;
INSERT INTO pairs SELECT m, 2 FROM generate_series(1, 5000) m;
INSERT INTO pairs SELECT m, 3 + m % 40 FROM generate_series(1, 5000) m ON CONFLICT DO NOTHING;
CREATE INDEX pairs_journal ON pairs USING sift_col (member, grp) WITH (track_changes = 'ctid');
CREATE TABLE by_member (id int PRIMARY KEY, member int, at timestamptz, n int NOT NULL, payload jsonb) WITH (autovacuum_enabled = false);
INSERT INTO by_member SELECT i, 1 + (i * 13) % 5000, timestamptz '2026-01-01' + i * interval '23 minutes', i % 80, jsonb_build_object('k', i % 9) FROM generate_series(1, 60000) i;
CREATE INDEX by_member_s ON by_member USING sift_col (member, at, id, n, payload) WITH (scope = 'grp: member -> pairs(member, grp)', track_changes = 'ctid');
CREATE TABLE by_time (id int PRIMARY KEY, member int, at timestamptz, n int NOT NULL, payload jsonb) WITH (autovacuum_enabled = false);
INSERT INTO by_time SELECT * FROM by_member;
CREATE INDEX by_time_s ON by_time USING sift_col (at, member, id, n, payload) WITH (scope = 'grp: member -> pairs(member, grp)', track_changes = 'ctid');
ANALYZE pairs;
ANALYZE by_member;
ANALYZE by_time;

CREATE FUNCTION answer(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text; plan text := ''; l text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP plan := plan || l; END LOOP;
	EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO b;
	RETURN (CASE WHEN a IS DISTINCT FROM b THEN 'DIFFERENT' ELSE 'same' END) || ' ' || (CASE WHEN plan LIKE '%SiftAggV6Scope%' THEN 'scope' ELSE 'normal' END) || ' ' || q;
END $$;
CREATE FUNCTION shapes(t text) RETURNS SETOF text LANGUAGE sql AS $$ SELECT format(q, t) FROM unnest(ARRAY[
	'SELECT count(*), sum(f.n) FROM %1$s f JOIN pairs s ON s.member = f.member WHERE s.grp = 1',
	'SELECT count(*), sum(f.n) FROM %1$s f JOIN pairs s ON s.member = f.member WHERE s.grp = 2',
	'SELECT count(*), sum(f.n) FROM %1$s f JOIN pairs s ON s.member = f.member WHERE s.grp = 1 AND f.at >= timestamptz ''2026-03-03 04:00'' AND f.at < timestamptz ''2026-03-29 11:00''',
	'SELECT count(*), sum(f.n) FROM %1$s f JOIN pairs s ON s.member = f.member WHERE s.grp = 2 AND f.at >= timestamptz ''2026-03-03 04:00'' AND f.at < timestamptz ''2026-03-29 11:00''',
	'SELECT count(*), sum(f.n) FROM %1$s f JOIN pairs s ON s.member = f.member WHERE s.grp = ANY(ARRAY[1, 2, 5])',
	'SELECT s.grp, count(*) FROM %1$s f JOIN pairs s ON s.member = f.member WHERE s.grp = ANY(ARRAY[1, 2, 5]) GROUP BY s.grp',
	'SELECT count(*), count(DISTINCT f.payload) FROM %1$s f JOIN pairs s ON s.member = f.member WHERE s.grp = 1',
	'SELECT f.payload->>''k'' AS k, count(*) FROM %1$s f JOIN pairs s ON s.member = f.member WHERE s.grp = 2 GROUP BY 1']) AS q $$;
SELECT answer(q) FROM shapes('by_member') q;
SELECT answer(q) FROM shapes('by_time') q;
-- the rolling window of every month boundary of the series
SELECT answer(format('SELECT count(*), sum(f.n) FROM by_member f JOIN pairs s ON s.member = f.member WHERE s.grp = 2 AND f.at >= timestamptz %L AND f.at < timestamptz %L', d::text, (d + interval '90 days')::text))
FROM generate_series(timestamptz '2026-01-01', timestamptz '2026-12-31', interval '47 days 7 hours') d;
DROP TABLE pairs, by_member, by_time;
DROP FUNCTION answer(text), shapes(text);
DROP EXTENSION pg_sift;
