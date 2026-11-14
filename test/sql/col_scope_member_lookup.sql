-- A patch reads the rows of the members whose pairs changed from the entity segments by member key. A text, varchar or uuid member is found there as an integer one is: the members are looked up in the
-- dictionary of the segment once, the zone and the leaf of each unit, which hold its codes, say which units hold a row of one, and the patch reads those units, not every unit of the segment (EXPLAIN:
-- Scope Patched Units). Only a unit that stores its strings inline (a string past 255 bytes) is read whole. Every read returns the rows of the join.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.col_unit_rows = 1000;
SET sift.agg_force = on;
SET sift.test_scope_patch = on;

-- 400 members, 100 facts each, one member for each day: the units of the entity rows (cut by day, 1,000 rows) hold ten members each, and the facts of the members that change are in ten of the 40 units.
-- The same data with an integer, text, varchar and uuid member, and with a text member of 300 bytes and more (every unit stores its strings inline)
CREATE TABLE i_events (id int PRIMARY KEY, m int, at date, kind int, amount int) WITH (autovacuum_enabled = false);
INSERT INTO i_events SELECT i, 1 + (i - 1) / 100, date '2024-01-01' + (i - 1) / 100, i % 4, i % 100 FROM generate_series(1, 40000) i;
CREATE TABLE i_pairs (m int NOT NULL, k int NOT NULL, PRIMARY KEY (m, k)) WITH (autovacuum_enabled = false);
INSERT INTO i_pairs SELECT m, 1 + m % 10 FROM generate_series(1, 400) m;
CREATE TABLE t_events (id int PRIMARY KEY, m text, at date, kind int, amount int) WITH (autovacuum_enabled = false);
INSERT INTO t_events SELECT id, 'acct-' || lpad(m::text, 4, '0'), at, kind, amount FROM i_events;
CREATE TABLE t_pairs (m text NOT NULL, k int NOT NULL, PRIMARY KEY (m, k)) WITH (autovacuum_enabled = false);
INSERT INTO t_pairs SELECT 'acct-' || lpad(m::text, 4, '0'), k FROM i_pairs;
CREATE TABLE v_events (id int PRIMARY KEY, m varchar(400), at date, kind int, amount int) WITH (autovacuum_enabled = false);
INSERT INTO v_events SELECT id, m, at, kind, amount FROM t_events;
CREATE TABLE u_events (id int PRIMARY KEY, m uuid, at date, kind int, amount int) WITH (autovacuum_enabled = false);
INSERT INTO u_events SELECT id, md5('acct-' || m)::uuid, at, kind, amount FROM i_events;
CREATE TABLE u_pairs (m uuid NOT NULL, k int NOT NULL, PRIMARY KEY (m, k)) WITH (autovacuum_enabled = false);
INSERT INTO u_pairs SELECT md5('acct-' || m)::uuid, k FROM i_pairs;
CREATE TABLE l_events (id int PRIMARY KEY, m text, at date, kind int, amount int) WITH (autovacuum_enabled = false);
INSERT INTO l_events SELECT id, repeat('long-', 60) || lpad(m::text, 4, '0'), at, kind, amount FROM i_events;
CREATE TABLE l_pairs (m text NOT NULL, k int NOT NULL, PRIMARY KEY (m, k)) WITH (autovacuum_enabled = false);
INSERT INTO l_pairs SELECT repeat('long-', 60) || lpad(m::text, 4, '0'), k FROM i_pairs;
-- the events of the last 50 days with members of 300 bytes: their units store the strings inline, the others share the dictionary of the segment
CREATE TABLE x_events (id int PRIMARY KEY, m text, at date, kind int, amount int) WITH (autovacuum_enabled = false);
INSERT INTO x_events SELECT id, CASE WHEN m > 350 THEN repeat('long-', 60) || lpad(m::text, 4, '0') ELSE 'acct-' || lpad(m::text, 4, '0') END, at, kind, amount FROM i_events;
CREATE TABLE x_pairs (m text NOT NULL, k int NOT NULL, PRIMARY KEY (m, k)) WITH (autovacuum_enabled = false);
INSERT INTO x_pairs SELECT CASE WHEN m > 350 THEN repeat('long-', 60) || lpad(m::text, 4, '0') ELSE 'acct-' || lpad(m::text, 4, '0') END, k FROM i_pairs;

CREATE INDEX i_pairs_j ON i_pairs USING sift_col (m, k) WITH (track_changes = 'ctid');
CREATE INDEX i_events_col ON i_events USING sift_col (id, m, at, kind, amount) WITH (key_cut = 'at', scope = 'k: m -> i_pairs(m, k)');
REINDEX INDEX i_events_col;
CREATE INDEX t_pairs_j ON t_pairs USING sift_col (m, k) WITH (track_changes = 'ctid');
CREATE INDEX t_events_col ON t_events USING sift_col (id, m, at, kind, amount) WITH (key_cut = 'at', scope = 'k: m -> t_pairs(m, k)');
REINDEX INDEX t_events_col;
CREATE INDEX u_pairs_j ON u_pairs USING sift_col (m, k) WITH (track_changes = 'ctid');
CREATE INDEX u_events_col ON u_events USING sift_col (id, m, at, kind, amount) WITH (key_cut = 'at', scope = 'k: m -> u_pairs(m, k)');
REINDEX INDEX u_events_col;
CREATE INDEX l_pairs_j ON l_pairs USING sift_col (m, k) WITH (track_changes = 'ctid');
CREATE INDEX l_events_col ON l_events USING sift_col (id, m, at, kind, amount) WITH (key_cut = 'at', scope = 'k: m -> l_pairs(m, k)');
REINDEX INDEX l_events_col;
CREATE INDEX x_pairs_j ON x_pairs USING sift_col (m, k) WITH (track_changes = 'ctid');
CREATE INDEX x_events_col ON x_events USING sift_col (id, m, at, kind, amount) WITH (key_cut = 'at', scope = 'k: m -> x_pairs(m, k)');
REINDEX INDEX x_events_col;
-- a varchar fact column of the text pairs
CREATE INDEX v_events_col ON v_events USING sift_col (id, m, at, kind, amount) WITH (key_cut = 'at', scope = 'k: m -> t_pairs(m, k)');
REINDEX INDEX v_events_col;
VACUUM (FREEZE, ANALYZE) i_events, i_pairs, t_events, t_pairs, v_events, u_events, u_pairs, l_events, l_pairs, x_events, x_pairs;

-- pairs of key 11 for ten members that are 40 apart: ten units of the 40 hold their rows
INSERT INTO i_pairs SELECT m, 11 FROM generate_series(7, 400, 40) m;
INSERT INTO t_pairs SELECT 'acct-' || lpad(m::text, 4, '0'), 11 FROM generate_series(7, 400, 40) m;
INSERT INTO u_pairs SELECT md5('acct-' || m)::uuid, 11 FROM generate_series(7, 400, 40) m;
INSERT INTO l_pairs SELECT repeat('long-', 60) || lpad(m::text, 4, '0'), 11 FROM generate_series(7, 400, 40) m;
INSERT INTO x_pairs SELECT CASE WHEN m > 350 THEN repeat('long-', 60) || lpad(m::text, 4, '0') ELSE 'acct-' || lpad(m::text, 4, '0') END, 11 FROM generate_series(7, 400, 40) m;

-- the read of key 11 as planned (the scope read with its patch), the rows of the join to compare, and what the patch read
CREATE FUNCTION patch_read(events text, pairs text, key int, OUT same boolean, OUT patched_keys int, OUT patched_rows int, OUT patched_units int, OUT entity_units int)
LANGUAGE plpgsql AS $$
DECLARE
	q text := format('SELECT kind, count(*), sum(amount) FROM %I e WHERE e.m IN (SELECT m FROM %I WHERE k = %s) GROUP BY kind ORDER BY kind', events, pairs, key);
	p jsonb;
	a text;
	b text;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) ' || q INTO p;
	EXECUTE 'SELECT coalesce(json_agg(t)::text, '''') FROM (' || q || ') t' INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT coalesce(json_agg(t)::text, '''') FROM (' || q || ') t' INTO b;
	SET LOCAL sift.enable_agg = on;
	same := a = b;
	patched_keys := coalesce(jsonb_path_query_first(p, '$.**."Scope Patched Keys"')::text::int, 0);
	patched_rows := coalesce(jsonb_path_query_first(p, '$.**."Scope Patched Rows"')::text::int, 0);
	patched_units := coalesce(jsonb_path_query_first(p, '$.**."Scope Patched Units"')::text::int, 0);
	SELECT units INTO entity_units FROM sift.col_segments((SELECT indexrelid::regclass FROM pg_index WHERE indrelid = events::regclass AND indexrelid::regclass::text LIKE '%\_col'))
		WHERE kind = 'immutable' ORDER BY gen LIMIT 1;
END $$;

SELECT label, r.* FROM (VALUES
	('integer member', 'i_events', 'i_pairs'),
	('text member', 't_events', 't_pairs'),
	('varchar facts, text pairs', 'v_events', 't_pairs'),
	('uuid member', 'u_events', 'u_pairs'),
	('text member of 300 bytes, units inline', 'l_events', 'l_pairs'),
	('text member, 50 days of the units inline', 'x_events', 'x_pairs')
) v (label, events, pairs), LATERAL patch_read(events, pairs, 11) r;

-- a member with a pair now and no fact is patched from no unit, a member whose last pair ended has nothing to read
INSERT INTO t_pairs VALUES ('acct-9999', 11), ('acct-0100', 11);
DELETE FROM t_pairs WHERE m = 'acct-0007' AND k = 11;
SELECT * FROM patch_read('t_events', 't_pairs', 11);

-- the member read by the statement (a count of its distinct values) is read from the rows of the patch too
CREATE FUNCTION distinct_same(events text, pairs text, key int) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE
	q text := format('SELECT count(DISTINCT m), count(*) FROM %I e WHERE e.m IN (SELECT m FROM %I WHERE k = %s)', events, pairs, key);
	a text;
	b text;
BEGIN
	EXECUTE 'SELECT row(t.*)::text FROM (' || q || ') t' INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT row(t.*)::text FROM (' || q || ') t' INTO b;
	SET LOCAL sift.enable_agg = on;
	RETURN a = b;
END $$;
SELECT label, distinct_same(events, pairs, 11) FROM (VALUES
	('integer member', 'i_events', 'i_pairs'),
	('text member', 't_events', 't_pairs'),
	('varchar facts, text pairs', 'v_events', 't_pairs'),
	('uuid member', 'u_events', 'u_pairs'),
	('text member of 300 bytes, units inline', 'l_events', 'l_pairs'),
	('text member, 50 days of the units inline', 'x_events', 'x_pairs')
) v (label, events, pairs);

DROP FUNCTION distinct_same(text, text, int);
DROP FUNCTION patch_read(text, text, int);
DROP TABLE i_events, i_pairs, t_events, t_pairs, v_events, u_events, u_pairs, l_events, l_pairs, x_events, x_pairs CASCADE;
DROP EXTENSION pg_sift;
