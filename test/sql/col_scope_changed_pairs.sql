-- A scope read through the changed pairs. The stored entries of the dim_a_id whose pairs changed since the scope segments were filled (added, removed, moved) leave the units at the source; the rows of
-- those dim_a_id are read once from the segments of the events, by dim_a_id, and are expanded by the pairs they have now, with the rows of the write buffers (the delta). The read of every key at once,
-- grouped by the key of each pair, and the read of several keys that counts a row for each pair, need no join for that. Table-driven: each state of the pairs and the events (the states accumulate) by each
-- read shape; every read is md5 equal to the plain join, takes the scope units (no "changed pairs" refusal), and reads no more rows than the changed dim_a_id have and the buffers hold: the rows read
-- from the segments are at most those the changed dim_a_id have, the copies of the delta at most those rows and the buffers' times the most keys a dim_a_id has. Counts only, no timings.
-- The testing markers stand for the recognition of the join: sift.in_scope_all and sift.scope_key (every key), sift.in_scope (one key or a list) and sift.in_scope_pairs (a row for each pair).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
-- dim_a_id in up to six keys on purpose: above the default limit of the multiplicity
SET sift.scope_max_multiplicity = 8;
SET jit = off;
SET client_min_messages = warning;
SET sift.agg_force = on;
-- the narrowed read is taken whatever it costs: the price is the subject of col_scope_patch_price
SET sift.test_scope_patch = on;

-- 12 keys; dim_a_id d is in 1 + d % 6 of them (1 to 6), 100..109 are in none, 301..320 have events and no pair; 20,000 padded events, some without a dim_a_id
CREATE TABLE tenant_scope (dim_a_id int, tenant_id int, PRIMARY KEY (dim_a_id, tenant_id));
INSERT INTO tenant_scope SELECT d, 1 + (d * 5 + j * 7) % 12 FROM generate_series(1, 300) d, generate_series(0, d % 6) j WHERE d NOT BETWEEN 100 AND 109 ON CONFLICT DO NOTHING;
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, amount int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, CASE WHEN i % 97 = 0 THEN NULL ELSE 1 + (i * 7919) % 320 END,
	timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, repeat('x', 100) FROM generate_series(1, 20000) i;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, amount)
	WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
REINDEX INDEX events_col;
VACUUM (FREEZE, ANALYZE) events;
VACUUM (ANALYZE) tenant_scope;

-- the rows of idx's table whose dim_a_id changed its pairs since the scope segments were filled (the journal says which), and the rows of its write buffers
CREATE FUNCTION changed_rows(tbl text, idx text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE
	t record;
	n bigint;
BEGIN
	SELECT lineage, horizon INTO t FROM sift._test_col_conn_stamps(idx::regclass) WHERE ordinal = 0 ORDER BY horizon::text::bigint LIMIT 1;
	EXECUTE format('SELECT count(*) FROM %I f WHERE f.dim_a_id IN (SELECT key FROM sift._test_scope_journal_changes(%L::regclass, 0, %s, %L::xid))', tbl, idx, t.lineage, t.horizon) INTO n;
	RETURN n;
END $$;

-- q: a read over the scope units, plain: the join. route: the fallback reason or "units"; delta: what the delta read (the rows of the buffers, those of the changed dim_a_id too: patch); bounded: the rows it read are within the bounds above (a read every key at once raises when the
-- reader refuses: the reason is the route then)
CREATE FUNCTION cp_run(q text, plain text, tbl text, idx text, OUT same boolean, OUT route text, OUT delta text, OUT bounded boolean) LANGUAGE plpgsql AS $$
DECLARE
	p jsonb;
	a text;
	b text;
	changed bigint := changed_rows(tbl, idx);
	buffered bigint;
	patched int;
	d int;
BEGIN
	SET LOCAL sift.enable_agg = on;
	BEGIN
		EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) ' || q INTO p;
		EXECUTE 'SELECT md5(coalesce(string_agg(t::text, ''|'' ORDER BY t::text), '''')) FROM (' || q || ') t' INTO a;
	EXCEPTION WHEN feature_not_supported THEN
		route := 'error: ' || substring(SQLERRM FROM '"(.*)"');
		RETURN;
	END;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT md5(coalesce(string_agg(t::text, ''|'' ORDER BY t::text), '''')) FROM (' || plain || ') t' INTO b;
	SET LOCAL sift.enable_agg = on;
	SELECT coalesce(sum(rows), 0) INTO buffered FROM sift.col_segments(idx::regclass) WHERE kind = 'buffer';
	same := a = b;
	route := coalesce(jsonb_path_query_first(p, '$.**."Scope Fallback"') #>> '{}', CASE WHEN jsonb_path_exists(p, '$.**."Scope Units Read"') THEN 'units' ELSE 'no scope read' END);
	patched := coalesce(jsonb_path_query_first(p, '$.**."Scope Patched Rows"')::text::int, 0);
	d := coalesce(jsonb_path_query_first(p, '$.**."Scope Delta Rows"')::text::int, 0);
	delta := CASE WHEN d = 0 THEN 'none' WHEN patched = 0 THEN 'buffers' ELSE 'patch' END;
	bounded := patched <= changed AND d <= (patched + buffered) * 6;
END $$;

-- the read shapes: the conjunct of the scope over the events f (%L its index), the aggregates, whether the statement groups by the key, and the join's form (pred: the condition over the pairs m, %s their relation)
CREATE TABLE shapes (n int, label text, scope text, aggs text, grouped boolean, joined boolean, pred text);
INSERT INTO shapes VALUES
	(1, 'every key, count and sum', 'sift.in_scope_all(%L::regclass, ''tenant_id'', f.dim_a_id)', 'count(*), sum(amount)', true, true, 'true'),
	(2, 'every key, count(DISTINCT dim_a_id)', 'sift.in_scope_all(%L::regclass, ''tenant_id'', f.dim_a_id)', 'count(DISTINCT f.dim_a_id), count(*)', true, true, 'true'),
	(3, 'every key, min and max', 'sift.in_scope_all(%L::regclass, ''tenant_id'', f.dim_a_id)', 'min(id), max(id), min(amount)', true, true, 'true'),
	(4, 'one key, count and sum', 'sift.in_scope(%L::regclass, ''tenant_id'', f.dim_a_id, 3)', 'count(*), sum(amount)', false, false,
		'f.dim_a_id IN (SELECT dim_a_id FROM %s m WHERE m.tenant_id = 3)'),
	(5, 'one key, min and max', 'sift.in_scope(%L::regclass, ''tenant_id'', f.dim_a_id, 4)', 'min(id), max(id)', false, false,
		'f.dim_a_id IN (SELECT dim_a_id FROM %s m WHERE m.tenant_id = 4)'),
	(6, 'a list of keys, a row once', 'sift.in_scope(%L::regclass, ''tenant_id'', f.dim_a_id, ''{3,5,7}''::int8[])', 'count(*), sum(amount)', false, false,
		'f.dim_a_id IN (SELECT dim_a_id FROM %s m WHERE m.tenant_id IN (3, 5, 7))'),
	(7, 'three keys, a row for each pair', 'sift.in_scope_pairs(%L::regclass, ''tenant_id'', f.dim_a_id, ''{3,5,7}''::int8[])', 'count(*), sum(amount), min(id)', false, true, 'm.tenant_id IN (3, 5, 7)');

-- tbl: the events, idx: their scope index, pairs: the relation of the pairs (a sub-select when its active pairs are a partial btree's)
CREATE FUNCTION probe(tbl text, idx text, pairs text) RETURNS TABLE (n int, label text, verdict text, route text, delta text, bounded boolean) LANGUAGE sql AS $$
	SELECT s.n, s.label, CASE r.same WHEN true THEN 'same' WHEN false THEN 'DIFFERENT' ELSE 'needs the join' END, r.route, r.delta, r.bounded
	FROM shapes s, LATERAL cp_run(
		format('SELECT %s%s FROM %I f WHERE %s%s', CASE WHEN s.grouped THEN format('sift.scope_key(%L::regclass, ''tenant_id'', f.dim_a_id) AS k, ', idx) END, s.aggs, tbl, format(s.scope, idx),
			CASE WHEN s.grouped THEN ' GROUP BY 1' END),
		format('SELECT %s%s FROM %I f %s WHERE %s%s', CASE WHEN s.grouped THEN 'm.tenant_id AS k, ' END, s.aggs, tbl, CASE WHEN s.joined THEN format('JOIN %s m ON m.dim_a_id = f.dim_a_id', pairs) END,
			CASE WHEN s.joined THEN s.pred ELSE format(s.pred, pairs) END, CASE WHEN s.grouped THEN ' GROUP BY 1' END),
		tbl, idx) r
	ORDER BY s.n
$$;

-- each state: the statements that make it (they accumulate), and then the reads; a pair that changed is a changed pair of the next reads until the index is rebuilt
CREATE TABLE states (n int, label text, stmt text);
INSERT INTO states VALUES
	(1, 'clean', 'SELECT 1'),
	(2, 'pairs added (dim_a_id with events, none yet in the key, and with no pair at all)',
		'INSERT INTO tenant_scope VALUES (1, 12), (2, 12), (7, 12), (301, 12), (302, 3), (303, 5), (303, 3) ON CONFLICT DO NOTHING'),
	(3, 'pairs removed', 'DELETE FROM tenant_scope WHERE tenant_id = 3 AND dim_a_id % 5 = 0'),
	(4, 'pairs moved (a dim_a_id changes tenant)',
		'WITH d AS (DELETE FROM tenant_scope WHERE dim_a_id % 7 = 0 RETURNING dim_a_id, tenant_id) INSERT INTO tenant_scope SELECT dim_a_id, 1 + tenant_id % 12 FROM d ON CONFLICT DO NOTHING'),
	(5, 'dim_a_id that lose every pair', 'DELETE FROM tenant_scope WHERE dim_a_id IN (8, 9, 10, 14)'),
	(6, 'buffered events of dim_a_id that lost every pair, of changed ones and of ones that did not change',
		'INSERT INTO events SELECT 20000 + i, CASE WHEN i % 3 = 0 THEN 8 + i % 3 WHEN i % 3 = 1 THEN 1 + i % 7 ELSE 20 + i % 40 END, timestamptz ''2026-02-05'', 4, ''u'' FROM generate_series(1, 150) i'),
	(7, 'promoted (the buffers are in the segments; the changed pairs still leave their dim_a_id out of the units)', 'SELECT sift.col_promote(''events_col'')'),
	(8, 'merged', 'SELECT sift.col_merge(''events_col'', true)'),
	(9, 'rebuilt (nothing changed since the stamp)', 'REINDEX INDEX events_col'),
	(10, 'a pair added after the rebuild, and a buffered event of it', 'INSERT INTO tenant_scope VALUES (5, 11), (6, 11) ON CONFLICT DO NOTHING; INSERT INTO events VALUES (30000, 5, timestamptz ''2026-02-06'', 1, ''v'')');
SELECT c FROM states, LATERAL unnest(ARRAY[format('SELECT %s AS n, %L AS state', n, label), stmt, 'SELECT * FROM probe(''events'', ''events_col'', ''tenant_scope'')']) WITH ORDINALITY u(c, o) ORDER BY n, o \gexec
SELECT sift.col_verify('events_col', true);

-- soft deleted pairs: the pairs are the rows of a partial unique btree, the journal index is plain and lists the soft deleted rows too, so the pairs of a dim_a_id are those of the btree: a restore, a
-- new active pair, a soft delete are changed pairs the read takes through the btree's predicate
CREATE TABLE sd_pairs (dim_a_id int, tenant_id int, deleted_at timestamptz, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO sd_pairs SELECT d, 1 + (d * 5 + j * 7) % 12, CASE WHEN (d + j) % 5 = 0 THEN now() END FROM generate_series(1, 200) d, generate_series(0, d % 3) j ON CONFLICT DO NOTHING;
CREATE UNIQUE INDEX sd_pairs_active ON sd_pairs (dim_a_id, tenant_id) WHERE deleted_at IS NULL;
CREATE INDEX sd_pairs_journal ON sd_pairs USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE sd_events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, amount int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO sd_events SELECT i, 1 + (i * 7919) % 220, timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, repeat('x', 100) FROM generate_series(1, 10000) i;
CREATE INDEX sd_events_col ON sd_events USING sift_col (id, dim_a_id, created_at, amount) WITH (scope = 'tenant_id: dim_a_id -> sd_pairs(dim_a_id, tenant_id)');
REINDEX INDEX sd_events_col;
VACUUM (FREEZE, ANALYZE) sd_events;

-- the shapes: how many answer as the join does, are read from the units, read a delta, and read within the bounds
CREATE FUNCTION sd_probe(OUT same int, OUT units int, OUT with_delta int, OUT bounded int) LANGUAGE sql AS $$
	SELECT count(*) FILTER (WHERE verdict = 'same'), count(*) FILTER (WHERE route = 'units'), count(*) FILTER (WHERE delta <> 'none'), count(*) FILTER (WHERE bounded)
	FROM probe('sd_events', 'sd_events_col', '(SELECT * FROM sd_pairs WHERE deleted_at IS NULL)')
$$;
CREATE TABLE sd_states (n int, label text, stmt text);
INSERT INTO sd_states VALUES
	(1, 'clean', 'SELECT 1'),
	(2, 'buffered events of dim_a_id with active and soft deleted pairs',
		'INSERT INTO sd_events SELECT 10000 + i, 1 + (i * 37) % 220, timestamptz ''2026-02-01'', 1, ''y'' FROM generate_series(1, 300) i'),
	(3, 'soft delete the pairs of dim_a_id that have buffered events', 'UPDATE sd_pairs SET deleted_at = now() WHERE dim_a_id % 6 = 1 AND deleted_at IS NULL'),
	(4, 'restore some', 'UPDATE sd_pairs SET deleted_at = NULL WHERE dim_a_id % 12 = 1'),
	(5, 'an active pair inserted for a dim_a_id with only soft deleted pairs', 'INSERT INTO sd_pairs SELECT dim_a_id, 12 FROM sd_pairs GROUP BY 1 HAVING bool_and(deleted_at IS NOT NULL) ON CONFLICT DO NOTHING'),
	(6, 'promoted', 'SELECT sift.col_promote(''sd_events_col'')'),
	(7, 'rebuilt', 'REINDEX INDEX sd_events_col');
SELECT c FROM sd_states, LATERAL unnest(ARRAY[format('SELECT %s AS n, %L AS state', n, label), stmt, 'SELECT * FROM sd_probe()']) WITH ORDINALITY u(c, o) ORDER BY n, o \gexec
DROP TABLE sd_states, sd_events, sd_pairs;
DROP FUNCTION sd_probe();

DROP TABLE shapes, states, events, tenant_scope;
DROP FUNCTION probe(text, text, text), cp_run(text, text, text, text), changed_rows(text, text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
