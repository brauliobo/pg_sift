-- An entity is in several scope keys (a membership table of (entity, group) pairs): a fact row has one to six scope entries. A read of one key sees each fact of a member that has the
-- pair once; a read of several keys that means "a pair with any of them" (IN, EXISTS) counts a fact once; a join counts it once for each pair. Every state is md5 equal to the plain
-- join (INTERFACE section 6b). Several keys are read from the scope units, the entry of the unit of the first key of the member counting (col_scope_dedupe measures it).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;
SET sift.agg_force = on;
SET sift.test_scope_trust = on;

CREATE FUNCTION scope_run(q text, plain text, OUT same boolean, OUT stored boolean, OUT why text, OUT semantics text)
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
	EXECUTE 'SELECT coalesce(json_agg(t)::text, '''') FROM (' || plain || ') t' INTO b;
	SET LOCAL sift.enable_agg = on;
	same := a = b;
	why := jsonb_path_query_first(p, '$.**."Scope Fallback"') #>> '{}';
	stored := jsonb_path_exists(p, '$.**."Scope Units Read"');
	semantics := jsonb_path_query_first(p, '$.**."Scope Semantics"') #>> '{}';
END $$;

-- 12 groups; member m is in 1 + m % 6 of them (1 to 6), members 100..109 are in none; 20,000 padded events, some without a member, some of a member that is nowhere
CREATE TABLE membership (dim_a_id int, group_id int, PRIMARY KEY (dim_a_id, group_id));
INSERT INTO membership SELECT m, 1 + (m * 5 + j * 7) % 12 FROM generate_series(1, 300) m, generate_series(0, m % 6) j WHERE m NOT BETWEEN 100 AND 109 ON CONFLICT DO NOTHING;
SELECT keys, count(*) AS entities FROM (SELECT dim_a_id, count(*) AS keys FROM membership GROUP BY 1) k GROUP BY 1 ORDER BY 1;
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int, at timestamptz, v int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO facts SELECT i, CASE WHEN i % 97 = 0 THEN NULL WHEN i % 89 = 0 THEN 999 ELSE 1 + (i * 7919) % 320 END,
	timestamptz '2026-01-01' + (i % 300) * interval '1 day', i % 10, repeat('x', 100) FROM generate_series(1, 20000) i;
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, at, v)
	WITH (scope = 'group_id: dim_a_id -> membership(dim_a_id, group_id)');
VACUUM (FREEZE, ANALYZE) facts;
VACUUM (ANALYZE) membership;

-- (a) one key: every fact of a member that has the pair, once (the join to the pair is unique)
SELECT k, r.same, r.stored, r.why
FROM generate_series(1, 13) k, LATERAL scope_run(
	format('SELECT count(*), sum(v), min(id), max(id) FROM facts f WHERE sift.in_scope(''facts_col''::regclass, ''group_id'', f.dim_a_id, %s)', k),
	format('SELECT count(*), sum(v), min(id), max(id) FROM facts f JOIN membership m ON m.dim_a_id = f.dim_a_id WHERE m.group_id = %s', k)) r
ORDER BY k;

-- (b) several keys with the meaning of IN / EXISTS: a fact once, though its member has pairs with several of the keys
CREATE TABLE sets (n int, keys int8[]);
INSERT INTO sets VALUES (1, '{2,9}'), (2, '{1,2,3,4,5,6}'), (3, '{7,9,11}'), (4, '{12,13}'), (5, '{5}'), (6, '{}');
SELECT s.n, s.keys, r.same, r.stored, r.why, r.semantics
FROM sets s, LATERAL scope_run(
	format('SELECT count(*), sum(v), min(id), max(id) FROM facts f WHERE sift.in_scope(''facts_col''::regclass, ''group_id'', f.dim_a_id, %L::int8[])', s.keys),
	format('SELECT count(*), sum(v), min(id), max(id) FROM facts f WHERE EXISTS (SELECT 1 FROM membership m WHERE m.dim_a_id = f.dim_a_id AND m.group_id = ANY (%L::int8[]))', s.keys)) r
ORDER BY s.n;
-- ... which is not the join: it counts a fact once for each pair, and the count of the join is the sum of the counts of the keys
SELECT (SELECT count(*) FROM facts f JOIN membership m ON m.dim_a_id = f.dim_a_id WHERE m.group_id IN (2, 9)) AS join_count,
	(SELECT count(*) FROM facts f WHERE sift.in_scope('facts_col'::regclass, 'group_id', f.dim_a_id, '{2,9}'::int8[])) AS once_count,
	(SELECT count(*) FROM facts f JOIN membership m ON m.dim_a_id = f.dim_a_id WHERE m.group_id = 2) +
	(SELECT count(*) FROM facts f JOIN membership m ON m.dim_a_id = f.dim_a_id WHERE m.group_id = 9) AS sum_of_keys;

-- (c) GROUP BY the key counts per pair (the join form: the planner lane recognises it, EXPLAIN says `Scope Semantics: per pair`)
SELECT group_id, count(*) FROM facts f JOIN membership m ON m.dim_a_id = f.dim_a_id GROUP BY 1 ORDER BY 1;
-- ... which is the read of each key, one key at a time
SELECT g AS group_id, (SELECT count(*) FROM facts f WHERE sift.in_scope('facts_col'::regclass, 'group_id', f.dim_a_id, g)) AS count
FROM generate_series(1, 12) g
EXCEPT
SELECT group_id, count(*) FROM facts f JOIN membership m ON m.dim_a_id = f.dim_a_id GROUP BY 1;

-- (e) facts deleted: the entry of the fact ends in every key of its member; a fact updated is a write buffer row until its promotion
DELETE FROM facts WHERE id % 11 = 0;
SELECT k, r.same, r.stored, r.why
FROM generate_series(1, 12, 3) k, LATERAL scope_run(
	format('SELECT count(*), sum(v) FROM facts f WHERE sift.in_scope(''facts_col''::regclass, ''group_id'', f.dim_a_id, %s)', k),
	format('SELECT count(*), sum(v) FROM facts f JOIN membership m ON m.dim_a_id = f.dim_a_id WHERE m.group_id = %s', k)) r
ORDER BY k;
VACUUM facts;
UPDATE facts SET v = v + 1 WHERE id % 7 = 0;
SELECT k, r.same, r.stored, r.why
FROM generate_series(1, 12, 3) k, LATERAL scope_run(
	format('SELECT count(*), sum(v) FROM facts f WHERE sift.in_scope(''facts_col''::regclass, ''group_id'', f.dim_a_id, %s)', k),
	format('SELECT count(*), sum(v) FROM facts f JOIN membership m ON m.dim_a_id = f.dim_a_id WHERE m.group_id = %s', k)) r
ORDER BY k;
SELECT sift.col_verify('facts_col', true);

-- PHASE 2: the journal vouches; (b) reads the units of each key and dedupes by heap address (a set bounded by work_mem, `Scope Fallback: dedupe over work_mem` past it); (d) a member moved
-- between keys (delete the pair (m, k1), insert (m, k2)) is in changed(S, X) once and read through the entity path for the keys that have the pair now, in every key it was in:
-- UPDATE membership SET group_id = 1 + group_id % 12 WHERE dim_a_id % 7 = 0;
-- <repeat (a) and (b)>

DROP TABLE sets, facts, membership;
DROP FUNCTION scope_run(text, text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
