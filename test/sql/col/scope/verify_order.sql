-- A scope segment holds a fact once for each of its scope keys, so its entries repeat an address: it has no global order (order_array_columns), which lists the rows of a
-- segment once, and col_verify stays clean on an index that names order arrays for the columns that the scope units replicate. The entity segments keep theirs. The shapes cut the
-- units by the member column, by the time and by nothing, and merge the runs of the build; each is checked against the plain join and read through the scope units.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
SET sift.agg_force = on;

CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
-- member m (1..600) is in (m % 4) of the 12 keys: 0..3
INSERT INTO tenant_scope SELECT m, 1 + (m * 5 + j * 3) % 12 FROM generate_series(1, 600) m, generate_series(0, 2) j WHERE j < m % 4;
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, kind int NOT NULL, value_num int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 1 + i % 600, timestamptz '2026-01-01' + i * interval '7 minutes', i % 7, i % 1000 FROM generate_series(1, 6000) i;

CREATE FUNCTION verdict(OUT verified boolean, OUT detail text) LANGUAGE plpgsql AS
$$ BEGIN
	PERFORM sift.col_verify('events_col', true);
	verified := true;
EXCEPTION WHEN OTHERS THEN
	verified := false;
	detail := SQLERRM;
END $$;
CREATE FUNCTION entries_are_the_join() RETURNS boolean LANGUAGE plpgsql AS
$$ BEGIN RETURN (SELECT md5(string_agg(tid::text || ':' || key, ',' ORDER BY tid, key)) FROM sift._test_col_scope_entries('events_col'))
	= (SELECT md5(string_agg(e.ctid::text || ':' || s.tenant_id, ',' ORDER BY e.ctid, s.tenant_id)) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id); END $$;
-- the segments that hold order arrays, of the scope and of the entities
CREATE FUNCTION order_arrays(OUT scope_segments_with bigint, OUT entity_segments_with bigint) LANGUAGE plpgsql AS
$$ BEGIN
	SELECT count(DISTINCT g.gen) FILTER (WHERE g.gen IN (SELECT gen FROM sift._test_col_scope_entries('events_col'))),
		   count(DISTINCT g.gen) FILTER (WHERE g.gen NOT IN (SELECT gen FROM sift._test_col_scope_entries('events_col')))
	INTO scope_segments_with, entity_segments_with
	FROM (SELECT gen FROM sift.col_segments('events_col') WHERE kind = 'immutable') g, LATERAL sift._test_col_sections('events_col', g.gen) s WHERE s.section = 'order_arrays';
END $$;
CREATE FUNCTION scope_read(OUT same boolean, OUT stored boolean) LANGUAGE plpgsql AS
$$ DECLARE
	p jsonb;
	a text;
	b text;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) SELECT id, created_at FROM events e WHERE sift.in_scope(''events_col''::regclass, ''tenant_id'', e.dim_a_id, 5) ORDER BY id LIMIT 40' INTO p;
	SELECT coalesce(json_agg(t)::text, '') INTO a FROM (SELECT id, created_at FROM events e WHERE sift.in_scope('events_col'::regclass, 'tenant_id', e.dim_a_id, 5) ORDER BY id LIMIT 40) t;
	SET LOCAL sift.enable_agg = off;
	SELECT coalesce(json_agg(t)::text, '') INTO b FROM (SELECT id, created_at FROM events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 5) ORDER BY id LIMIT 40) t;
	SET LOCAL sift.enable_agg = on;
	same := a = b;
	stored := jsonb_path_exists(p, '$.**."Scope Units Read"');
END $$;

CREATE TABLE shapes (n serial, name text, setting text, opts text);
INSERT INTO shapes (name, setting, opts) VALUES
	('default',      'sift.test_conn_fill = 0',        ''),
	('cut by member', 'sift.test_conn_fill = 0',        ', key_cut = ''dim_a_id'''),
	('cut by time',   'sift.test_conn_fill = 0',        ', key_cut = ''created_at, kind'''),
	('many runs',     'sift.test_col_segment_kb = 64', ''),
	('many runs, cut by member', 'sift.test_col_segment_kb = 64', ', key_cut = ''dim_a_id''');
CREATE FUNCTION build(shape int, OUT verified boolean, OUT detail text, OUT entries_are_the_join boolean, OUT scope_segments_with bigint, OUT entity_segments_with bigint) LANGUAGE plpgsql AS
$$ DECLARE
	s shapes;
BEGIN
	SELECT * INTO s FROM shapes WHERE n = shape;
	EXECUTE 'SET ' || s.setting;
	DROP INDEX IF EXISTS events_col;
	EXECUTE 'CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, value_num) WITH (order_array_columns = ''id, created_at, kind, value_num'', scope = ''tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)''' || s.opts || ')';
	RESET sift.test_conn_fill;
	RESET sift.test_col_segment_kb;
	SELECT v.verified, v.detail INTO verified, detail FROM verdict() v;
	entries_are_the_join := entries_are_the_join();
	SELECT o.scope_segments_with, o.entity_segments_with INTO scope_segments_with, entity_segments_with FROM order_arrays() o;
END $$;
SELECT s.name, b.verified, b.detail, b.entries_are_the_join, b.scope_segments_with, b.entity_segments_with > 0 AS entities_keep_theirs FROM shapes s, LATERAL build(s.n) b ORDER BY s.n;

-- the last shape reads the scope units of a key as the join does (the journal starts at the REINDEX)
REINDEX INDEX events_col;
VACUUM (FREEZE, ANALYZE) events;
VACUUM (ANALYZE) tenant_scope;
SELECT * FROM scope_read();

-- the writers that follow the build make scope segments of their own: a promotion and the merge of everything
INSERT INTO events SELECT i, 1 + i % 600, timestamptz '2026-06-01' + i * interval '7 minutes', i % 7, i % 1000 FROM generate_series(6001, 6400) i;
SELECT sift.col_promote('events_col') >= 0 AS promoted;
SELECT * FROM verdict();
SELECT sift.col_merge('events_col', true) >= 0 AS merged;
SELECT * FROM verdict();
SELECT entries_are_the_join() AS entries_are_the_join, (SELECT scope_segments_with FROM order_arrays()) AS scope_segments_with;

DROP TABLE events, tenant_scope CASCADE;
DROP TABLE shapes;
DROP FUNCTION verdict(), entries_are_the_join(), order_arrays(), scope_read(), build(int);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
