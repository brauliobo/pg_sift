-- The build of a scope index writes, besides the entity segments, scope segments (col/build/scope.c): key-cut segments whose entries are the fact rows replicated once for each
-- scope key of their member, each unit holding the rows of one scope key or the whole runs of several small ones, never an address twice. The multiset of (address, scope key) of the scope segments is the join of the fact rows with the scope relation,
-- for members in 0..6 keys, rows with a NULL or an unknown member, skewed members; the segments of the entities are as without the scope (rows counted once). Keys of a few rows
-- The scope relation carries its journal index (a scope index needs it); the stamps are not looked at here.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;

CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
-- member m (1..1500) is in (m % 7) of the 40 keys: 0..6; members 1..20 are the hot ones of the facts below
INSERT INTO tenant_scope SELECT m, 1 + (m * 31 + j * 13) % 40 FROM generate_series(1, 1500) m, generate_series(0, 5) j WHERE j < m % 7;
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, kind int NOT NULL, value_num int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO events
	SELECT i, CASE WHEN i % 17 = 0 THEN NULL WHEN i % 19 = 0 THEN 100000 + i WHEN i % 3 = 0 THEN 1 + i % 20 ELSE 1 + (i * 7919) % 1500 END,
		   CASE WHEN i % 23 = 0 THEN NULL ELSE timestamptz '2026-01-01' + i * interval '7 minutes' END, i % 7, i % 1000
	FROM generate_series(1, 9000) i;

-- plpgsql, so that nothing refers to the index before it exists
CREATE FUNCTION entries_md5() RETURNS text LANGUAGE plpgsql AS
$$ BEGIN RETURN (SELECT md5(string_agg(tid::text || ':' || key, ',' ORDER BY tid, key)) FROM sift._test_col_scope_entries('events_col')); END $$;
CREATE FUNCTION join_md5() RETURNS text LANGUAGE plpgsql AS
$$ BEGIN RETURN (SELECT md5(string_agg(e.ctid::text || ':' || s.tenant_id, ',' ORDER BY e.ctid, s.tenant_id)) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id); END $$;
-- what the segments hold, and the shape of the units
CREATE FUNCTION shape(OUT entity_rows bigint, OUT scope_segments bigint, OUT scope_entries bigint, OUT repeated_addresses bigint, OUT dead_entries bigint) LANGUAGE plpgsql AS
$$ BEGIN
	entity_rows := (SELECT rows FROM sift.col_index_info('events_col'));
	scope_segments := (SELECT count(DISTINCT gen) FROM sift._test_col_scope_entries('events_col'));
	scope_entries := (SELECT count(*) FROM sift._test_col_scope_entries('events_col'));
	repeated_addresses := (SELECT count(*) FROM (SELECT gen, unit FROM sift._test_col_scope_entries('events_col') GROUP BY 1, 2 HAVING count(*) > count(DISTINCT tid)) t);
	dead_entries := (SELECT count(*) FROM sift._test_col_scope_entries('events_col') x LEFT JOIN events e ON e.ctid = x.tid WHERE e.id IS NULL);
END $$;

-- build shapes: the way the pairs are known (relation read once / btree probed), the memory of a run (many runs merged by the build), the cut of the entities
CREATE TABLE shapes (n serial, name text, setting text, opts text);
INSERT INTO shapes (name, setting, opts) VALUES
	('default',             'sift.test_conn_fill = 0',       ''),
	('key set',             'sift.test_conn_fill = 1',       ''),
	('probe',               'sift.test_conn_fill = 2',       ''),
	('many runs',           'sift.test_col_segment_kb = 64', ''),
	('dim_a cut by key', 'sift.test_conn_fill = 0',       ', key_cut = ''created_at, kind'''),
	('many runs, cut',      'sift.test_col_segment_kb = 64', ', key_cut = ''created_at, kind''');
CREATE FUNCTION build(shape int, OUT entries_are_the_join boolean, OUT entity_rows_once boolean, OUT has_scope_segments boolean, OUT repeated_addresses bigint, OUT dead_entries bigint,
					  OUT entry_count boolean) LANGUAGE plpgsql AS
$$ DECLARE
	s shapes;
	f record;
BEGIN
	SELECT * INTO s FROM shapes WHERE n = shape;
	EXECUTE 'SET ' || s.setting;
	DROP INDEX IF EXISTS events_col;
	EXECUTE 'CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, value_num) WITH (scope = ''tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)''' || s.opts || ')';
	RESET sift.test_conn_fill;
	RESET sift.test_col_segment_kb;
	entries_are_the_join := entries_md5() = join_md5();
	SELECT * INTO f FROM shape();
	entity_rows_once := f.entity_rows = (SELECT count(*) FROM events);
	has_scope_segments := f.scope_segments > 0;
	repeated_addresses := f.repeated_addresses;
	dead_entries := f.dead_entries;
	entry_count := f.scope_entries = (SELECT count(*) FROM events e JOIN tenant_scope t ON t.dim_a_id = e.dim_a_id);
END $$;
SELECT s.name, b.* FROM shapes s, LATERAL build(s.n) b ORDER BY s.n;

-- the keys of a fact: 0 for the NULL and unknown members, else 1..6
CREATE FUNCTION keys_per_fact() RETURNS TABLE (n bigint, facts bigint) LANGUAGE plpgsql AS
$$ BEGIN RETURN QUERY SELECT coalesce(c.n, 0), count(*) FROM events e LEFT JOIN (SELECT tid, count(*) AS n FROM sift._test_col_scope_entries('events_col') GROUP BY 1) c ON c.tid = e.ctid GROUP BY 1 ORDER BY 1; END $$;
SELECT * FROM keys_per_fact();
SELECT sift.col_verify('events_col', true);

-- the scope segments have no point directory, which the entity segments have: a read of one key does not find a row by its value
SELECT g.gen IN (SELECT gen FROM sift._test_col_scope_entries('events_col')) AS scope_segment, bool_or(s.section = 'point_dir') AS has_point_directory
	FROM (SELECT gen FROM sift.col_segments('events_col') WHERE kind = 'immutable') g, LATERAL sift._test_col_sections('events_col', g.gen) s GROUP BY 1 ORDER BY 1;

-- the scope stamps are written with every segment (a scope relation has no journal in this round: lineage 0, the horizon of the snapshot the pairs were read under)
SELECT count(*) > 0 AS stamped, count(*) FILTER (WHERE lineage <> 0) AS vouched, count(*) FILTER (WHERE horizon::text::bigint = 0) AS without_horizon
	FROM sift._test_col_conn_stamps('events_col');

-- a promotion writes the scope segments of its rows beside its entity segment, and the merge of everything joins them: the entries stay those of the join (col_scope_maint has the rest)
INSERT INTO events SELECT i, 1 + i % 1500, timestamptz '2026-06-01' + i * interval '7 minutes', i % 7, i % 1000 FROM generate_series(9001, 9400) i;
SELECT sift.col_promote('events_col') >= 0 AS promoted;
SELECT entries_md5() = join_md5() AS entries_are_the_join_promoted;
SELECT sift.col_merge('events_col', true) >= 0 AS merged;
SELECT entries_md5() = join_md5() AS entries_are_the_join_merged;
SELECT (shape()).entity_rows = (SELECT count(*) FROM events) AS entity_rows_once, (shape()).repeated_addresses AS repeated_addresses;
SELECT sift.col_verify('events_col', true);

-- a fact that is deleted or updated has no live entry in any of its keys after VACUUM, the entries of the others are untouched (the entries are by address: an update writes a new address)
SELECT entries_md5() = join_md5() AS entries_before_edits;
CREATE TEMP TABLE gone AS SELECT ctid AS tid, id FROM events WHERE id % 5 = 0 OR id % 7 = 1;
DELETE FROM events WHERE id % 5 = 0;
UPDATE events SET value_num = value_num + 1 WHERE id % 7 = 1;
VACUUM events;
SELECT count(*) AS entries_of_dead_addresses FROM sift._test_col_scope_entries('events_col') x JOIN gone g ON g.tid = x.tid;
SELECT (shape()).dead_entries AS dead_entries, (shape()).repeated_addresses AS repeated_addresses;
SELECT count(*) AS surviving_entries_not_in_the_join
	FROM sift._test_col_scope_entries('events_col') x JOIN events e ON e.ctid = x.tid LEFT JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id AND s.tenant_id = x.key WHERE s.dim_a_id IS NULL;
SELECT sift.col_verify('events_col', true);

-- an index without the scope has no scope segment, and a REINDEX of the scope index builds them again
DROP INDEX events_col;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, value_num);
SELECT (shape()).scope_segments AS scope_segments;
ALTER INDEX events_col SET (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
REINDEX INDEX events_col;
SELECT (shape()).scope_segments > 0 AS has_scope_segments, entries_md5() = join_md5() AS entries_are_the_join;

-- keys of a few rows share units: 300 keys of 10 entries (the members of the first 10 keys are also in the next of them, so these keys share facts and each has a unit of its own)
-- are in far fewer units than keys, each address once in a unit, and the entries are still the join
CREATE TABLE small_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO small_scope SELECT m, 1 + m % 300 FROM generate_series(1, 3000) m;
INSERT INTO small_scope SELECT m, 1 + (m + 1) % 10 FROM generate_series(1, 3000) m WHERE m % 300 < 10;
CREATE INDEX small_scope_journal ON small_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE small_events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, kind int NOT NULL, value_num int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO small_events SELECT i, 1 + (i * 7) % 3000, timestamptz '2026-01-01' + i * interval '7 minutes', i % 7, i % 1000 FROM generate_series(1, 3000) i;
CREATE INDEX small_col ON small_events USING sift_col (id, dim_a_id, created_at, kind, value_num) WITH (scope = 'tenant_id: dim_a_id -> small_scope(dim_a_id, tenant_id)');
SELECT sift.col_verify('small_col', true);
SELECT (SELECT count(DISTINCT key) FROM sift._test_col_scope_entries('small_col')) AS keys, count(*) < 20 AS far_fewer_units, count(*) FILTER (WHERE n > 1) > 0 AS units_shared_by_keys
	FROM (SELECT count(DISTINCT key) AS n FROM sift._test_col_scope_entries('small_col') GROUP BY gen, unit) t;
SELECT count(*) AS repeated_addresses FROM (SELECT gen, unit FROM sift._test_col_scope_entries('small_col') GROUP BY 1, 2 HAVING count(*) > count(DISTINCT tid)) t;
SELECT (SELECT md5(string_agg(tid::text || ':' || key, ',' ORDER BY tid, key)) FROM sift._test_col_scope_entries('small_col')) =
	   (SELECT md5(string_agg(e.ctid::text || ':' || s.tenant_id, ',' ORDER BY e.ctid, s.tenant_id)) FROM small_events e JOIN small_scope s ON s.dim_a_id = e.dim_a_id) AS entries_are_the_join;

DROP TABLE small_events;
DROP TABLE small_scope;
DROP TABLE events;
DROP TABLE tenant_scope;
DROP FUNCTION build(int);
DROP FUNCTION entries_md5();
DROP FUNCTION join_md5();
DROP FUNCTION shape();
DROP FUNCTION keys_per_fact();
DROP TABLE shapes;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
