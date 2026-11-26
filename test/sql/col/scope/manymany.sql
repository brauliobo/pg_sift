-- An entity is in several scope keys (a membership table of (entity, group) pairs): a fact row has one to six pairs. A read of one key sees each fact of a member that has the pair once; a read of
-- several keys that means "a pair with any of them" (IN, EXISTS) counts a fact once; a join counts it once for each pair. Every state is md5 equal through the scope node, through sift with
-- sift.agg_scope off and through the normal plan (INTERFACE section 6b), and says how the node read it: members moved between keys, facts deleted and updated, two scopes of one index, a list
-- of many keys and a small work_mem.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
-- members in up to six keys on purpose: above the default limit of the multiplicity
SET jit = off;
SET client_min_messages = warning;
-- the scope summary is Format 2.0; the testing library builds new indexes in Format 1.0 when told to
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;

-- the same rows through every plan, and the Scope Read of the node (none: not a scope read)
CREATE FUNCTION scope_run(q text, OUT same boolean, OUT route text)
LANGUAGE plpgsql AS $$
DECLARE
	p jsonb;
	a text;
	b text;
	c text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) ' || q INTO p;
	EXECUTE 'SELECT md5(coalesce(string_agg(t::text, ''|'' ORDER BY t::text), '''')) FROM (' || q || ') t' INTO a;
	SET LOCAL sift.agg_scope = off;
	EXECUTE 'SELECT md5(coalesce(string_agg(t::text, ''|'' ORDER BY t::text), '''')) FROM (' || q || ') t' INTO b;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT md5(coalesce(string_agg(t::text, ''|'' ORDER BY t::text), '''')) FROM (' || q || ') t' INTO c;
	SET LOCAL sift.enable_agg = on;
	SET LOCAL sift.agg_scope = on;
	same := a = b AND b = c;
	route := coalesce(jsonb_path_query_first(p, '$.**."Scope Read"') #>> '{}', 'none');
END $$;

-- 12 groups; member m is in 1 + m % 6 of them (1 to 6), members 100..109 are in none; 20,000 padded events, some without a member, some of a member that is nowhere
CREATE TABLE membership (dim_a_id int, group_id int, PRIMARY KEY (dim_a_id, group_id));
INSERT INTO membership SELECT m, 1 + (m * 5 + j * 7) % 12 FROM generate_series(1, 300) m, generate_series(0, m % 6) j WHERE m NOT BETWEEN 100 AND 109 ON CONFLICT DO NOTHING;
SELECT keys, count(*) AS dim_a FROM (SELECT dim_a_id, count(*) AS keys FROM membership GROUP BY 1) k GROUP BY 1 ORDER BY 1;
CREATE INDEX membership_journal ON membership USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid');
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int, at timestamptz, v int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO facts SELECT i, CASE WHEN i % 97 = 0 THEN NULL WHEN i % 89 = 0 THEN 999 ELSE 1 + (i * 7919) % 320 END,
	timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, repeat('x', 100) FROM generate_series(1, 20000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, at, v)
	WITH (scope = 'group_id: dim_a_id -> membership(dim_a_id, group_id) max_multiplicity(8)');
REINDEX INDEX facts_col;
VACUUM (FREEZE, ANALYZE) facts;
VACUUM (ANALYZE) membership;

-- (a) one key: every fact of a member that has the pair, once (the join to the pair is unique)
SELECT k, r.same, r.route
FROM generate_series(1, 13) k, LATERAL scope_run(
	format('SELECT count(*), sum(v), min(id), max(id) FROM facts f JOIN membership m ON m.dim_a_id = f.dim_a_id WHERE m.group_id = %s', k)) r
ORDER BY k;

-- (b) several keys with the meaning of IN / EXISTS: a fact once, though its member has pairs with several of the keys
CREATE TABLE sets (n int, keys int8[]);
INSERT INTO sets VALUES (1, '{2,9}'), (2, '{1,2,3,4,5,6}'), (3, '{7,9,11}'), (4, '{12,13}'), (5, '{5}'), (6, '{}');
SELECT s.n, s.keys, r.same, r.route
FROM sets s, LATERAL scope_run(
	format('SELECT count(*), sum(v), min(id), max(id) FROM facts f WHERE EXISTS (SELECT 1 FROM membership m WHERE m.dim_a_id = f.dim_a_id AND m.group_id = ANY (%L::int8[]))', s.keys)) r
ORDER BY s.n;
-- ... which is not the join: it counts a fact once for each pair, and the count of the join is the sum of the counts of the keys
SELECT (SELECT count(*) FROM facts f JOIN membership m ON m.dim_a_id = f.dim_a_id WHERE m.group_id IN (2, 9)) AS join_count,
	(SELECT count(*) FROM facts f WHERE EXISTS (SELECT 1 FROM membership m WHERE m.dim_a_id = f.dim_a_id AND m.group_id IN (2, 9))) AS once_count,
	(SELECT count(*) FROM facts f JOIN membership m ON m.dim_a_id = f.dim_a_id WHERE m.group_id = 2) +
	(SELECT count(*) FROM facts f JOIN membership m ON m.dim_a_id = f.dim_a_id WHERE m.group_id = 9) AS sum_of_keys;

-- (c) GROUP BY the key counts per pair
SELECT group_id, count(*) FROM facts f JOIN membership m ON m.dim_a_id = f.dim_a_id GROUP BY 1 ORDER BY 1;
-- ... which is the read of each key, one key at a time
SELECT g AS group_id, (SELECT count(*) FROM facts f JOIN membership m ON m.dim_a_id = f.dim_a_id WHERE m.group_id = g) AS count
FROM generate_series(1, 12) g
EXCEPT
SELECT group_id, count(*) FROM facts f JOIN membership m ON m.dim_a_id = f.dim_a_id GROUP BY 1;

-- (d) a member moved between keys, and (b) stays exact for it: the journal of the pair relation says which members changed
UPDATE membership SET group_id = 1 + group_id % 12 WHERE dim_a_id % 7 = 0 AND NOT EXISTS (SELECT 1 FROM membership o WHERE o.dim_a_id = membership.dim_a_id AND o.group_id = 1 + membership.group_id % 12);
SELECT s.n, s.keys, r.same, r.route
FROM sets s, LATERAL scope_run(
	format('SELECT count(*), sum(v), min(id), max(id) FROM facts f WHERE EXISTS (SELECT 1 FROM membership m WHERE m.dim_a_id = f.dim_a_id AND m.group_id = ANY (%L::int8[]))', s.keys)) r
ORDER BY s.n;

-- (e) facts deleted: the fact leaves every key of its member; a fact updated is a write buffer row until its promotion
DELETE FROM facts WHERE id % 11 = 0;
SELECT k, r.same, r.route
FROM generate_series(1, 12, 3) k, LATERAL scope_run(
	format('SELECT count(*), sum(v) FROM facts f JOIN membership m ON m.dim_a_id = f.dim_a_id WHERE m.group_id = %s', k)) r
ORDER BY k;
VACUUM facts;
UPDATE facts SET v = v + 1 WHERE id % 7 = 0;
SELECT k, r.same, r.route
FROM generate_series(1, 12, 3) k, LATERAL scope_run(
	format('SELECT count(*), sum(v) FROM facts f JOIN membership m ON m.dim_a_id = f.dim_a_id WHERE m.group_id = %s', k)) r
ORDER BY k;
SELECT sift.col_verify('facts_col', true);


-- two scopes of one index: a group filter and a region filter on the facts of the same member are an intersection
CREATE TABLE region_scope (dim_a_id int, region_id int, PRIMARY KEY (dim_a_id, region_id));
INSERT INTO region_scope SELECT m, 1 + m % 4 FROM generate_series(1, 300) m;
CREATE INDEX region_scope_journal ON region_scope USING sift_col (dim_a_id, region_id) WITH (track_changes = 'ctid');
CREATE TABLE facts2 AS SELECT * FROM facts WHERE id <= 5000;
CREATE INDEX facts2_col ON facts2 USING sift_col (id, dim_a_id, at, v)
	WITH (scope = 'group_id: dim_a_id -> membership(dim_a_id, group_id) max_multiplicity(8); region_id: dim_a_id -> region_scope(dim_a_id, region_id) max_multiplicity(8)');
REINDEX INDEX facts2_col;
SELECT r.same, r.route FROM scope_run(
	'SELECT count(*), sum(v) FROM facts2 f WHERE f.dim_a_id IN (SELECT dim_a_id FROM membership WHERE group_id = 3) AND f.dim_a_id IN (SELECT dim_a_id FROM region_scope WHERE region_id = 2)') r;
SELECT r.same, r.route FROM scope_run(
	'SELECT count(*), sum(v) FROM facts2 f WHERE f.dim_a_id IN (SELECT dim_a_id FROM region_scope WHERE region_id IN (1, 3))') r;
DROP TABLE facts2, region_scope;

-- lists of many keys, and members of several keys under a small work_mem
CREATE TABLE wide_pairs (dim_a_id int, group_id int, PRIMARY KEY (dim_a_id, group_id));
INSERT INTO wide_pairs SELECT m, 1 + m % 200 FROM generate_series(1, 400) m UNION SELECT m, 1 + (m + 7) % 200 FROM generate_series(1, 400) m;
CREATE INDEX wide_journal ON wide_pairs USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid');
CREATE TABLE wide_facts AS SELECT i AS id, 1 + i % 400 AS dim_a_id, timestamptz '2026-01-01' + i * interval '1 hour' AS at, i % 10 AS v FROM generate_series(1, 3000) i;
CREATE INDEX wide_col ON wide_facts USING sift_col (id, dim_a_id, at, v) WITH (scope = 'group_id: dim_a_id -> wide_pairs(dim_a_id, group_id) max_multiplicity(8)');
REINDEX INDEX wide_col;
SELECT n, r.same, r.route FROM (VALUES (1, (SELECT array_agg(g)::int8[] FROM generate_series(1, 40) g)), (2, (SELECT array_agg(g)::int8[] FROM generate_series(1, 100) g))) k(n, keys),
LATERAL scope_run(
	format('SELECT count(*), sum(v) FROM wide_facts f WHERE EXISTS (SELECT 1 FROM wide_pairs m WHERE m.dim_a_id = f.dim_a_id AND m.group_id = ANY (%L::int8[]))', k.keys)) r ORDER BY n;
DROP TABLE wide_facts, wide_pairs;
CREATE TABLE heavy_pairs (dim_a_id int, group_id int, PRIMARY KEY (dim_a_id, group_id));
INSERT INTO heavy_pairs SELECT m, g FROM generate_series(1, 2000) m, generate_series(1, 3) g;
CREATE INDEX heavy_journal ON heavy_pairs USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid');
CREATE TABLE heavy_facts AS SELECT i AS id, 1 + i % 2000 AS dim_a_id, timestamptz '2026-01-01' + i * interval '1 hour' AS at, i % 10 AS v FROM generate_series(1, 4000) i;
CREATE INDEX heavy_col ON heavy_facts USING sift_col (id, dim_a_id, at, v) WITH (scope = 'group_id: dim_a_id -> heavy_pairs(dim_a_id, group_id) max_multiplicity(8)');
REINDEX INDEX heavy_col;
SELECT r.same, r.route FROM scope_run(
	'SELECT count(*), sum(v) FROM heavy_facts f WHERE EXISTS (SELECT 1 FROM heavy_pairs m WHERE m.dim_a_id = f.dim_a_id AND m.group_id IN (1, 2, 3))') r;
SET work_mem = '64kB';
SELECT r.same, r.route FROM scope_run(
	'SELECT count(*), sum(v) FROM heavy_facts f WHERE EXISTS (SELECT 1 FROM heavy_pairs m WHERE m.dim_a_id = f.dim_a_id AND m.group_id IN (1, 2, 3))') r;
RESET work_mem;
DROP TABLE heavy_facts, heavy_pairs;

DROP TABLE sets, facts, membership;
DROP FUNCTION scope_run(text);
DROP EXTENSION pg_sift;
