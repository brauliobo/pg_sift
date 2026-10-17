-- SiftScan on materialized views; the statements that cannot use it say so; LIMIT $1 of a generic plan; SERIALIZABLE; RLS
CREATE EXTENSION pg_sift;
\set VERBOSITY terse

CREATE TABLE pc (id int, owner text, body text);
INSERT INTO pc SELECT i, CASE WHEN i % 2 = 0 THEN 'sift_t_even' ELSE 'sift_t_odd' END,
	'alpha ' || repeat('beta ', i % 5) || i FROM generate_series(1, 2000) i;
CREATE INDEX pc_idx ON pc USING sift (body);
VACUUM ANALYZE pc;

-- materialized views get the ranked scan and the count
CREATE MATERIALIZED VIEW pcm AS SELECT id, body FROM pc;
CREATE INDEX pcm_idx ON pcm USING sift (body);
ANALYZE pcm;
EXPLAIN (COSTS OFF) SELECT id FROM pcm WHERE body @@> 'beta' ORDER BY sift.score(ctid) DESC LIMIT 3;
EXPLAIN (COSTS OFF) SELECT count(*) FROM pcm WHERE body @@> 'beta 17';
SELECT id FROM pcm WHERE body @@> 'beta' ORDER BY sift.score(ctid) DESC, id LIMIT 3;
SELECT count(*) FROM pcm WHERE body @@> 'beta 17';
INSERT INTO pc VALUES (5000, 'x', 'alpha beta beta beta beta zzz');
REFRESH MATERIALIZED VIEW pcm;
SELECT id FROM pcm WHERE body @@> 'zzz' ORDER BY sift.score(ctid) DESC LIMIT 3;

-- rows that are locked, updated or deleted have no SiftScan to rank them
SELECT id FROM pc WHERE body @@> 'zzz' FOR UPDATE;
SELECT id FROM pc WHERE body @@> 'zzz' ORDER BY sift.score(ctid) DESC LIMIT 1 FOR UPDATE;
UPDATE pc SET owner = owner WHERE body @@> 'zzz';
UPDATE pc SET owner = owner WHERE body @@> 'zzz' RETURNING sift.score(ctid);
UPDATE pc SET id = id WHERE body @@> 'zzz' AND sift.score(ctid) > 0;
DELETE FROM pc WHERE body @@> 'nothing here' RETURNING sift.score(ctid);
-- ranking in a SELECT and changing the rows by key works
UPDATE pc SET owner = owner WHERE ctid = (SELECT ctid FROM pc WHERE body @@> 'zzz' ORDER BY sift.score(ctid) DESC LIMIT 1);

-- LIMIT and OFFSET of a generic plan are read by the scan when it starts
CREATE FUNCTION limit_hint(q text) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, FORMAT JSON, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q INTO j;
	RETURN QUERY SELECT v::text FROM jsonb_path_query(j, 'strict $.**."Limit Hint"') v;
END $$;
PREPARE lim(text, int8, int8) AS
	SELECT id FROM pc WHERE body @@> $1 ORDER BY sift.score(ctid) DESC LIMIT $2 OFFSET $3;
SET plan_cache_mode = force_generic_plan;
SELECT limit_hint('EXECUTE lim(''beta'', 5, 20)');
SELECT limit_hint('EXECUTE lim(''beta'', 100, 0)');
SELECT limit_hint('EXECUTE lim(''beta'', NULL, 0)');
EXECUTE lim('beta', 3, 2);
EXPLAIN (COSTS OFF) EXECUTE lim('beta', 3, 2);
RESET plan_cache_mode;
SET plan_cache_mode = force_custom_plan;
SELECT limit_hint('EXECUTE lim(''beta'', 5, 20)');
EXECUTE lim('beta', 3, 2);
RESET plan_cache_mode;

-- SERIALIZABLE: the same answers (the scan locks the whole relation: DESIGN.md)
BEGIN ISOLATION LEVEL SERIALIZABLE;
SELECT id FROM pc WHERE body @@> 'zzz' ORDER BY sift.score(ctid) DESC LIMIT 3;
SELECT count(*) FROM pc WHERE body @@> 'beta 17';
SELECT count(*) FROM pcm WHERE body @@> 'zzz';
COMMIT;

-- row-level security: a role sees its own rows
DROP ROLE IF EXISTS sift_t_even, sift_t_odd;
CREATE ROLE sift_t_even;
CREATE ROLE sift_t_odd;
GRANT USAGE ON SCHEMA sift TO sift_t_even, sift_t_odd;
GRANT SELECT ON pc TO sift_t_even, sift_t_odd;
ALTER TABLE pc ENABLE ROW LEVEL SECURITY;
CREATE POLICY pc_own ON pc USING (owner = current_user);
SET ROLE sift_t_even;
SELECT count(*), min(id), max(id) FROM pc WHERE body @@> 'beta';
SELECT id FROM pc WHERE body @@> 'beta' ORDER BY id LIMIT 3;
-- the scan would count the hidden rows in the score and the estimates: no scan, no score
SELECT id FROM pc WHERE body @@> 'beta' ORDER BY sift.score(ctid) DESC, id LIMIT 3;
EXPLAIN (COSTS OFF) SELECT count(*) FROM pc WHERE body @@> 'beta';
RESET ROLE;
SET ROLE sift_t_odd;
SELECT count(*), min(id), max(id) FROM pc WHERE body @@> 'beta';
RESET ROLE;
ALTER TABLE pc DISABLE ROW LEVEL SECURITY;

DROP FUNCTION limit_hint(text);
DROP MATERIALIZED VIEW pcm;
DROP TABLE pc;
DROP OWNED BY sift_t_even, sift_t_odd;
DROP ROLE sift_t_even, sift_t_odd;
DROP EXTENSION pg_sift;
