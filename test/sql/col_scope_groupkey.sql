-- GROUP BY the scope key, "every tenant": a scan of every key of a scope reads the scope units key by key, a fact counted once for each pair of its member (the join's meaning), and groups by the derived leaf
-- "<key_name>#scope", which each row it reads has. A read of the write buffers is expanded by the pairs of their members (the delta); the stored entries of a member whose pairs changed leave, and the rows of
-- such a member are read once from the entity segments and expanded by the pairs it has now, so that every row has its key (col_scope_changed_pairs: what that reads). Table-driven: each state of the facts and
-- the pairs (the states accumulate) by each shape (aggregates, filters, members in one, three or no key); the statement is the join `f JOIN pairs m ON m.member = f.fk GROUP BY m.key` as an application writes it (the
-- planner recognises it: agg_scope_group_key), and every read is md5 equal to the join under sift.agg_scope = off and to the normal plan (the group rows included), from the units or, where the reader refuses,
-- from the join the node runs (the route says why).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
-- members in up to six keys on purpose: above the default limit of the multiplicity
SET sift.scope_max_multiplicity = 8;
SET jit = off;
SET client_min_messages = warning;
SET sift.agg_force = on;

\ir test/scope_helpers.sql

-- 12 groups; member m is in 1 + m % 6 of them (1 to 6), members 100..109 are in none, members 301..320 have facts and no pair; 20,000 padded facts, some without a member
CREATE TABLE membership (dim_a_id int, group_id int, PRIMARY KEY (dim_a_id, group_id));
INSERT INTO membership SELECT m, 1 + (m * 5 + j * 7) % 12 FROM generate_series(1, 300) m, generate_series(0, m % 6) j WHERE m NOT BETWEEN 100 AND 109 ON CONFLICT DO NOTHING;
CREATE INDEX membership_journal ON membership USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid');
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int, at timestamptz, v int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO facts SELECT i, CASE WHEN i % 97 = 0 THEN NULL ELSE 1 + (i * 7919) % 320 END,
	timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, repeat('x', 100) FROM generate_series(1, 20000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, at, v)
	WITH (scope = 'group_id: dim_a_id -> membership(dim_a_id, group_id)');
REINDEX INDEX facts_col;
VACUUM (FREEZE, ANALYZE) facts;
VACUUM (ANALYZE) membership;

-- the read shapes: the aggregates, a condition on a stored column or none, and whether the statement groups by the key
CREATE TABLE shapes (n int, label text, aggs text, filter text, grouped boolean);
INSERT INTO shapes VALUES
	(1, 'count and sum per key', 'count(*), sum(v)', NULL, true),
	(2, 'count(DISTINCT member) per key', 'count(DISTINCT f.dim_a_id), count(*)', NULL, true),
	(3, 'min and max per key', 'min(id), max(id), min(v)', NULL, true),
	(4, 'count and sum, filter on a stored column', 'count(*), sum(v)', 'f.v > 4', true),
	(5, 'min and max, filter on the time', 'min(id), max(id)', 'f.at >= timestamptz ''2026-04-01''', true),
	(6, 'dim_b in one key', 'count(*), sum(v)', 'f.dim_a_id % 6 = 0', true),
	(7, 'dim_b in three keys', 'count(*), sum(v)', 'f.dim_a_id % 6 = 2', true),
	(8, 'dim_b in no key', 'count(*), sum(v)', 'f.dim_a_id BETWEEN 100 AND 109', true),
	(9, 'a row for each pair, no group', 'count(*), sum(v), min(id), max(id)', NULL, false);

-- facts: the table, pairs: the relation of the pairs (a sub-select when its active pairs are a partial btree's)
CREATE FUNCTION probe(facts text, pairs text)
RETURNS TABLE (n int, label text, verdict text, route text, group_key boolean, preagg boolean, delta text) LANGUAGE sql AS $$
	SELECT s.n, s.label, CASE r.same WHEN true THEN 'same' ELSE 'DIFFERENT' END, coalesce(r.why, CASE WHEN r.stored THEN 'units' END), r.group_key, r.preagg,
		CASE WHEN r.delta_rows IS NULL THEN 'no read' WHEN r.delta_rows = 0 THEN 'none' ELSE 'read' END
	FROM shapes s, LATERAL scope_group_run(
		format('SELECT %s%s FROM %s f JOIN %s m ON m.dim_a_id = f.dim_a_id%s%s', CASE WHEN s.grouped THEN 'm.group_id AS k, ' END, s.aggs, facts, pairs,
			coalesce(' WHERE ' || s.filter, ''), CASE WHEN s.grouped THEN ' GROUP BY 1' END)) r
	ORDER BY s.n
$$;

-- each state: the statement that makes it (they accumulate), and then the reads
CREATE TABLE states (n int, label text, stmt text);
INSERT INTO states VALUES
	(1, 'buffered inserts, 1% of the table (dim_b of any number of keys, of none, of no member)',
		'INSERT INTO facts SELECT 20000 + i, CASE WHEN i % 50 = 0 THEN NULL ELSE 1 + (i * 37) % 320 END, timestamptz ''2026-02-01'', 1, ''y'' FROM generate_series(1, 200) i'),
	(2, 'buffered updates (the old versions dead in their units) and deletes (stored rows and buffered ones)',
		'UPDATE facts SET v = v + 100 WHERE id % 9 = 0 AND id <= 20000; DELETE FROM facts WHERE id % 11 = 0 AND id <= 20000; DELETE FROM facts WHERE id > 20000 AND id % 7 = 0'),
	(3, 'promoted (the buffers are in the segments)',
		'SELECT sift.col_promote(''facts_col'')'),
	(4, 'buffered inserts after the promotion',
		'INSERT INTO facts SELECT 22000 + i, 1 + (i * 17) % 320, timestamptz ''2026-02-03'', 3, ''w'' FROM generate_series(1, 150) i'),
	(5, 'merged',
		'SELECT sift.col_merge(''facts_col'', true)'),
	(6, 'dim_b that lose every pair, after a rebuild (their stored entries leave, no entity row needs a key)',
		'REINDEX INDEX facts_col; DELETE FROM membership WHERE dim_a_id IN (8, 9, 10, 14)'),
	(7, 'buffered rows of dim_b that lose every pair, and of dim_b that did not change',
		'INSERT INTO facts SELECT 24000 + i, CASE WHEN i % 4 = 0 THEN 8 + i % 3 ELSE 20 + i % 40 END, timestamptz ''2026-02-05'', 4, ''u'' FROM generate_series(1, 120) i; DELETE FROM membership WHERE dim_a_id IN (20, 21)'),
	(8, 'pairs added, after a rebuild (dim_b that have buffered rows too)',
		'REINDEX INDEX facts_col; INSERT INTO membership VALUES (1, 12), (2, 12), (7, 12), (301, 12), (302, 3), (303, 5), (303, 3) ON CONFLICT DO NOTHING'),
	(9, 'pairs removed, after a rebuild',
		'REINDEX INDEX facts_col; DELETE FROM membership WHERE group_id = 3 AND dim_a_id % 5 = 0'),
	(10, 'pairs moved (a member changes tenant), after a rebuild',
		'REINDEX INDEX facts_col; WITH d AS (DELETE FROM membership WHERE dim_a_id % 7 = 0 RETURNING dim_a_id, group_id) INSERT INTO membership SELECT dim_a_id, 1 + group_id % 12 FROM d ON CONFLICT DO NOTHING'),
	(11, 'promoted (the pairs that changed still leave their dim_b out of the units)',
		'SELECT sift.col_promote(''facts_col'')'),
	(12, 'merged',
		'SELECT sift.col_merge(''facts_col'', true)'),
	(13, 'rebuilt',
		'REINDEX INDEX facts_col');
CREATE FUNCTION apply(i int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	s states;
BEGIN
	SELECT * INTO s FROM states WHERE n = i;
	EXECUTE s.stmt;
	RETURN s.label;
END $$;

\echo clean
SELECT * FROM probe('facts', 'membership');
SELECT c FROM states, LATERAL unnest(ARRAY[format('SELECT apply(%s) AS state', n), 'SELECT * FROM probe(''facts'', ''membership'')']) WITH ORDINALITY u(c, o) ORDER BY n, o \gexec
SELECT sift.col_verify('facts_col', true);

-- soft deleted pairs: the pairs are the rows of a partial unique btree, the journal index is plain and lists the soft deleted rows too, so the pairs of a buffered row's member are those of the btree. A buffered row of a member
-- with only soft deleted pairs reads nothing, one of a member with some active pairs a copy for each of them; every shape's answer is the join's on the active pairs through each state
CREATE TABLE sd_pairs (dim_a_id int, group_id int, deleted_at timestamptz, PRIMARY KEY (dim_a_id, group_id)) WITH (autovacuum_enabled = false);
INSERT INTO sd_pairs SELECT m, 1 + (m * 5 + j * 7) % 12, CASE WHEN (m + j) % 5 = 0 THEN now() END FROM generate_series(1, 200) m, generate_series(0, m % 3) j ON CONFLICT DO NOTHING;
CREATE UNIQUE INDEX sd_pairs_active ON sd_pairs (dim_a_id, group_id) WHERE deleted_at IS NULL;
CREATE INDEX sd_pairs_journal ON sd_pairs USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid');
CREATE TABLE sd_facts (id int PRIMARY KEY, dim_a_id int, at timestamptz, v int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO sd_facts SELECT i, 1 + (i * 7919) % 220, timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, repeat('x', 100) FROM generate_series(1, 10000) i;
CREATE INDEX sd_facts_col ON sd_facts USING sift_col (id, dim_a_id, at, v) WITH (scope = 'group_id: dim_a_id -> sd_pairs(dim_a_id, group_id)');
REINDEX INDEX sd_facts_col;
VACUUM (FREEZE, ANALYZE) sd_facts;

-- the shapes: how many answer as the join does, are read from the units, say the key and read a delta, and how many the node answers with the join
CREATE FUNCTION sd_probe(OUT same int, OUT units int, OUT with_key int, OUT with_delta int, OUT needs_join int) LANGUAGE sql AS $$
	SELECT count(*) FILTER (WHERE verdict = 'same'), count(*) FILTER (WHERE route = 'units'), count(*) FILTER (WHERE group_key),
		count(*) FILTER (WHERE delta = 'read'), count(*) FILTER (WHERE route <> 'units')
	FROM probe('sd_facts', '(SELECT * FROM sd_pairs WHERE deleted_at IS NULL)')
$$;
CREATE TABLE sd_states (n int, label text, stmt text);
INSERT INTO sd_states VALUES
	(1, 'clean', 'SELECT 1'),
	(2, 'buffered inserts of dim_b with active and soft deleted pairs',
		'INSERT INTO sd_facts SELECT 10000 + i, 1 + (i * 37) % 220, timestamptz ''2026-02-01'', 1, ''y'' FROM generate_series(1, 300) i'),
	(3, 'soft delete the pairs of dim_b that have buffered rows', 'UPDATE sd_pairs SET deleted_at = now() WHERE dim_a_id % 6 = 1 AND deleted_at IS NULL'),
	(4, 'restore some', 'UPDATE sd_pairs SET deleted_at = NULL WHERE dim_a_id % 12 = 1'),
	(5, 'promoted', 'SELECT sift.col_promote(''sd_facts_col'')');
SELECT c FROM sd_states, LATERAL unnest(ARRAY[format('SELECT %s AS n, %L AS state', n, label), stmt, 'SELECT * FROM sd_probe()']) WITH ORDINALITY u(c, o) ORDER BY n, o \gexec
DROP TABLE sd_states, sd_facts, sd_pairs;
DROP FUNCTION sd_probe();

DROP TABLE shapes, states, facts, membership;
DROP FUNCTION probe(text, text), apply(int), scope_group_run(text), scope_run(text, text), scope_reason_ok(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
