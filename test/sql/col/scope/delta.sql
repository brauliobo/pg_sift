-- The rows of the write buffers have no place in the scope summary yet. A read of a scope key answers from the summary and the changes since it (Scope Cube: delta), or from the join when the
-- summary is not exact for its snapshot. Table-driven: each state of the facts and the pairs (the states accumulate), each read shape; every read is md5 equal to the heap (sift.enable_agg = off)
-- and says how the scope node read it. The joins of one key or of several keys (a row for each pair) are scope reads; IN and EXISTS are read by the plain node.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
-- the scope summary is Format 2.0; the testing library builds new indexes in Format 1.0 when told to
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;

-- q read by the node and by the heap: equal or not, the Scope Read of the node (NULL: not a scope read) and its Scope Cube
CREATE FUNCTION scope_run(q text, OUT same boolean, OUT route text, OUT cube text)
LANGUAGE plpgsql AS $$
DECLARE
	p jsonb;
	a text;
	b text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) ' || q INTO p;
	EXECUTE 'SELECT coalesce(json_agg(t)::text, '''') FROM (' || q || ') t' INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT coalesce(json_agg(t)::text, '''') FROM (' || q || ') t' INTO b;
	SET LOCAL sift.enable_agg = on;
	same := a = b;
	route := jsonb_path_query_first(p, '$.**."Scope Read"') #>> '{}';
	cube := jsonb_path_query_first(p, '$.**."Scope Cube"') #>> '{}';
END $$;

-- 12 groups; member m is in 1 + m % 6 of them (1 to 6), members 100..109 are in none, members 301..320 have facts and no pair; 20,000 padded facts, some without a member
CREATE TABLE membership (dim_a_id int, group_id int, PRIMARY KEY (dim_a_id, group_id));
INSERT INTO membership SELECT m, 1 + (m * 5 + j * 7) % 12 FROM generate_series(1, 300) m, generate_series(0, m % 6) j WHERE m NOT BETWEEN 100 AND 109 ON CONFLICT DO NOTHING;
CREATE INDEX membership_journal ON membership USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid');
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int, at timestamptz, v int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO facts SELECT i, CASE WHEN i % 97 = 0 THEN NULL ELSE 1 + (i * 7919) % 320 END,
	timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, repeat('x', 100) FROM generate_series(1, 20000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, at, v)
	WITH (scope = 'group_id: dim_a_id -> membership(dim_a_id, group_id) max_multiplicity(8)');
REINDEX INDEX facts_col;
VACUUM (FREEZE, ANALYZE) facts;
VACUUM (ANALYZE) membership;

-- the read shapes, $ the keys
CREATE TABLE shapes (n int, label text, keys int8[], q text);
INSERT INTO shapes VALUES
	(1, 'one key, count and sum', '{3}', 'SELECT count(*), sum(v), min(id), max(id) FROM facts f JOIN membership m ON m.dim_a_id = f.dim_a_id WHERE m.group_id = ANY ($)'),
	(2, 'IN list of keys', '{7,9,11}', 'SELECT count(*), sum(v), min(id), max(id) FROM facts f WHERE f.dim_a_id IN (SELECT dim_a_id FROM membership WHERE group_id = ANY ($))'),
	(3, 'EXISTS, dim_b in several keys', '{1,2,3,4,5,6}',
		'SELECT count(*), sum(v), min(id), max(id) FROM facts f WHERE EXISTS (SELECT 1 FROM membership m WHERE m.dim_a_id = f.dim_a_id AND m.group_id = ANY ($))'),
	(4, 'a row for each pair of three keys', '{3,5,7}', 'SELECT count(*), sum(v), min(id), max(id) FROM facts f JOIN membership m ON m.dim_a_id = f.dim_a_id WHERE m.group_id = ANY ($)'),
	(5, 'listing of one key', '{4}', 'SELECT id, v FROM facts f JOIN membership m ON m.dim_a_id = f.dim_a_id WHERE m.group_id = ANY ($) ORDER BY id DESC LIMIT 25');

CREATE FUNCTION probe() RETURNS TABLE (n int, label text, verdict text, route text, cube text) LANGUAGE sql AS $$
	SELECT s.n, s.label, CASE WHEN r.same THEN 'same' ELSE 'DIFFERENT' END, coalesce(r.route, 'not a scope read'), r.cube
	FROM shapes s, LATERAL scope_run(replace(s.q, '$', format('%L::int8[]', s.keys))) r
	ORDER BY s.n
$$;

-- each state: the statement that makes it (they accumulate), and then the reads
CREATE TABLE states (n int, label text, stmt text);
INSERT INTO states VALUES
	(1, 'buffered inserts, 1% of the table (dim_b of any number of keys, of none, of no member)',
		'INSERT INTO facts SELECT 20000 + i, CASE WHEN i % 50 = 0 THEN NULL ELSE 1 + (i * 37) % 320 END, timestamptz ''2026-02-01'', 1, ''y'' FROM generate_series(1, 200) i'),
	(2, 'buffered updates (the old versions dead in their units) and deletes (stored rows and buffered ones)',
		'UPDATE facts SET v = v + 100 WHERE id % 9 = 0 AND id <= 20000; DELETE FROM facts WHERE id % 11 = 0 AND id <= 20000; DELETE FROM facts WHERE id > 20000 AND id % 7 = 0'),
	(3, 'pairs added (dim_b that have buffered rows too)',
		'INSERT INTO membership VALUES (1, 12), (2, 12), (7, 12), (301, 12), (302, 3), (303, 5), (303, 3) ON CONFLICT DO NOTHING'),
	(4, 'pairs removed',
		'DELETE FROM membership WHERE group_id = 3 AND dim_a_id % 5 = 0'),
	(5, 'pairs moved (a member changes tenant)',
		'WITH d AS (DELETE FROM membership WHERE dim_a_id % 7 = 0 RETURNING dim_a_id, group_id) INSERT INTO membership SELECT dim_a_id, 1 + group_id % 12 FROM d ON CONFLICT DO NOTHING'),
	(6, 'buffered rows of a changed member, and of dim_b that did not change',
		'INSERT INTO facts SELECT 21000 + i, 1 + (i * 13) % 320, timestamptz ''2026-02-02'', 2, ''z'' FROM generate_series(1, 300) i; DELETE FROM membership WHERE dim_a_id IN (8, 9, 10, 14); INSERT INTO membership VALUES (8, 5), (9, 4), (14, 11) ON CONFLICT DO NOTHING'),
	(7, 'promoted (the buffers are in the segments; the pairs that changed still patch)',
		'SELECT sift.col_promote(''facts_col'')'),
	(8, 'buffered inserts after the promotion',
		'INSERT INTO facts SELECT 22000 + i, 1 + (i * 17) % 320, timestamptz ''2026-02-03'', 3, ''w'' FROM generate_series(1, 150) i'),
	(9, 'merged',
		'SELECT sift.col_merge(''facts_col'', true)');
CREATE FUNCTION apply(i int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	s states;
BEGIN
	SELECT * INTO s FROM states WHERE n = i;
	EXECUTE s.stmt;
	RETURN s.label;
END $$;

\echo clean
SELECT * FROM probe();
SELECT c FROM states, LATERAL unnest(ARRAY[format('SELECT apply(%s) AS state', n), 'SELECT * FROM probe()']) WITH ORDINALITY u(c, o) ORDER BY n, o \gexec
SELECT sift.col_verify('facts_col', true);

-- soft deleted pairs: the pairs are the rows of a partial unique btree, the journal index is plain and lists the soft deleted rows too. A buffered row of a member with only soft deleted pairs
-- reads nothing, one of a member with some active pairs reads a row for each of them; every key's answer is the heap's on the active pairs, through each state of the pairs and the buffers
CREATE TABLE sd_pairs (dim_a_id int, group_id int, deleted_at timestamptz, PRIMARY KEY (dim_a_id, group_id)) WITH (autovacuum_enabled = false);
INSERT INTO sd_pairs SELECT m, 1 + (m * 5 + j * 7) % 12, CASE WHEN (m + j) % 5 = 0 THEN now() END FROM generate_series(1, 200) m, generate_series(0, m % 3) j ON CONFLICT DO NOTHING;
CREATE UNIQUE INDEX sd_pairs_active ON sd_pairs (dim_a_id, group_id) WHERE deleted_at IS NULL;
CREATE INDEX sd_pairs_journal ON sd_pairs USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid');
CREATE TABLE sd_facts (id int PRIMARY KEY, dim_a_id int, at timestamptz, v int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO sd_facts SELECT i, 1 + (i * 7919) % 220, timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, repeat('x', 100) FROM generate_series(1, 10000) i;
CREATE INDEX sd_facts_col ON sd_facts USING sift_col (id, dim_a_id, at, v) WITH (scope = 'group_id: dim_a_id -> sd_pairs(dim_a_id, group_id) max_multiplicity(8)');
REINDEX INDEX sd_facts_col;
VACUUM (FREEZE, ANALYZE) sd_facts;

-- the groups 1..12 one at a time: how many answer as the heap does, and how many are scope reads
CREATE FUNCTION sd_probe(OUT same int, OUT scope_reads int) LANGUAGE sql AS $$
	SELECT count(*) FILTER (WHERE r.same), count(*) FILTER (WHERE r.route IS NOT NULL)
	FROM generate_series(1, 12) k, LATERAL scope_run(
		format('SELECT count(*), sum(v), min(id), max(id) FROM sd_facts f JOIN sd_pairs p ON p.dim_a_id = f.dim_a_id WHERE p.group_id = %s AND p.deleted_at IS NULL', k)) r
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
DROP FUNCTION probe(), apply(int), scope_run(text);
DROP EXTENSION pg_sift;
