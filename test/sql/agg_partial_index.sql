-- A partial sift_col index (WHERE deleted_at IS NULL AND day >= '2026-01-15') answers a query whose conditions imply its predicate, since it holds
-- exactly the rows that pass it; a query that does not imply it keeps the normal plan. Results with the scan off and on are the same after every
-- change of the heap that moves rows in and out of the predicate.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE facts (id int, grp int, day date, deleted_at timestamp, score float8, doc jsonb);
INSERT INTO facts
SELECT i, i % 7, date '2026-01-01' + i % 60, CASE WHEN i % 10 = 0 THEN timestamp '2026-03-01' END, i % 13, jsonb_build_object('k', i % 5)
FROM generate_series(1, 30000) i;
CREATE TABLE dims (id int, kind text);
INSERT INTO dims SELECT i, CASE WHEN i % 2 = 0 THEN 'a' ELSE 'b' END FROM generate_series(0, 6) i;
CREATE INDEX facts_col ON facts USING sift_col (id, grp, day, deleted_at, score, doc) WITH (paths = 'doc.k int4')
  WHERE deleted_at IS NULL AND day >= '2026-01-15';
VACUUM (FREEZE, ANALYZE) facts;
ANALYZE dims;
-- The result, or the SQLSTATE and message, with the scan off and on. on: the table scans are off, so a Custom Scan (SiftAgg) runs when the query is one it can run.
\ir test/agg_helpers.sql
CREATE FUNCTION check_query(name text, q text) RETURNS TABLE (label text, scan bool, same bool, outcome text) LANGUAGE plpgsql AS $$
BEGIN
  RETURN QUERY SELECT name, runs_in_scan(q), outcome(q, false) = outcome(q, true), left(outcome(q, true), 100);
END $$;
CREATE FUNCTION check_all() RETURNS TABLE (label text, scan bool, same bool, outcome text) LANGUAGE sql AS $$
 SELECT * FROM check_query('implied', 'SELECT grp, count(*), sum(score) FROM facts WHERE deleted_at IS NULL AND day >= ''2026-02-01'' GROUP BY grp')
 UNION ALL SELECT * FROM check_query('same predicate', 'SELECT count(*), count(DISTINCT grp), avg(score) FROM facts WHERE deleted_at IS NULL AND day >= ''2026-01-15''')
 UNION ALL SELECT * FROM check_query('between', 'SELECT grp, count(*) FROM facts WHERE deleted_at IS NULL AND day BETWEEN ''2026-02-01'' AND ''2026-02-10'' GROUP BY grp')
 UNION ALL SELECT * FROM check_query('path', 'SELECT (doc -> ''k'')::int AS k, count(*) FROM facts WHERE deleted_at IS NULL AND day >= ''2026-01-20'' AND grp < 5 GROUP BY 1')
 UNION ALL SELECT * FROM check_query('join', 'SELECT d.kind, count(*) FROM facts f JOIN dims d ON d.id = f.grp WHERE f.deleted_at IS NULL AND f.day >= ''2026-01-15'' GROUP BY 1')
 UNION ALL SELECT * FROM check_query('left join', 'SELECT d.kind, count(f.id) FROM dims d LEFT JOIN facts f ON f.grp = d.id AND f.deleted_at IS NULL AND f.day >= ''2026-03-01'' GROUP BY 1')
 UNION ALL SELECT * FROM check_query('without the day', 'SELECT grp, count(*) FROM facts WHERE deleted_at IS NULL GROUP BY grp')
 UNION ALL SELECT * FROM check_query('without the deleted_at', 'SELECT grp, count(*) FROM facts WHERE day >= ''2026-02-01'' GROUP BY grp')
 UNION ALL SELECT * FROM check_query('earlier day', 'SELECT grp, count(*) FROM facts WHERE deleted_at IS NULL AND day >= ''2026-01-10'' GROUP BY grp')
 UNION ALL SELECT * FROM check_query('or', 'SELECT grp, count(*) FROM facts WHERE deleted_at IS NULL AND (day >= ''2026-02-01'' OR grp = 3) GROUP BY grp')
 UNION ALL SELECT * FROM check_query('no condition', 'SELECT grp, count(*) FROM facts GROUP BY grp')
$$;
SELECT * FROM check_all();
-- rows move into and out of the predicate: new rows on both sides, a row deleted_at is set on, one it is cleared on, a day moved across the bound
INSERT INTO facts SELECT 30000 + i, i % 7, date '2026-01-01' + i % 60, CASE WHEN i % 4 = 0 THEN timestamp '2026-03-02' END, i % 11, jsonb_build_object('k', i % 3) FROM generate_series(1, 4000) i;
UPDATE facts SET deleted_at = timestamp '2026-03-03' WHERE id % 50 = 1 AND deleted_at IS NULL;
UPDATE facts SET deleted_at = NULL WHERE id % 50 = 0 AND deleted_at IS NOT NULL;
UPDATE facts SET day = day - 40 WHERE id % 70 = 2;
UPDATE facts SET day = day + 40 WHERE id % 70 = 3;
UPDATE facts SET score = score + 1 WHERE id % 9 = 4;
DELETE FROM facts WHERE id % 111 = 5;
SELECT * FROM check_all();
BEGIN;
UPDATE facts SET deleted_at = timestamp '2026-03-04' WHERE id % 20 = 7;
INSERT INTO facts SELECT 40000 + i, 1, date '2026-02-01', NULL, 1, '{}' FROM generate_series(1, 500) i;
ROLLBACK;
SELECT * FROM check_all();
VACUUM facts;
SELECT * FROM check_all();
REINDEX INDEX facts_col;
SELECT * FROM check_all();
-- a prepared statement: a custom plan folds the parameter into a constant that implies the predicate, a generic plan cannot know it
CREATE FUNCTION check_prepared(mode text) RETURNS TABLE (label text, scan bool, same bool) LANGUAGE plpgsql AS $$
DECLARE n_off bigint; n_on bigint; diff bigint;
BEGIN
  PERFORM set_config('plan_cache_mode', mode, true);
  PERFORM set_config('sift.enable_agg', 'off', true);
  EXECUTE 'DEALLOCATE ALL';
  EXECUTE 'PREPARE by_day (date) AS SELECT grp, count(*) AS n FROM facts WHERE deleted_at IS NULL AND day >= $1 GROUP BY grp';
  EXECUTE 'CREATE TEMP TABLE r_off ON COMMIT DROP AS EXECUTE by_day(''2026-02-01'')';
  PERFORM set_config('sift.enable_agg', 'on', true);
  PERFORM set_config(s, 'false', true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
  EXECUTE 'DEALLOCATE ALL';
  EXECUTE 'PREPARE by_day (date) AS SELECT grp, count(*) AS n FROM facts WHERE deleted_at IS NULL AND day >= $1 GROUP BY grp';
  EXECUTE 'CREATE TEMP TABLE r_on ON COMMIT DROP AS EXECUTE by_day(''2026-02-01'')';
  SELECT count(*) INTO diff FROM ((TABLE r_off EXCEPT TABLE r_on) UNION ALL (TABLE r_on EXCEPT TABLE r_off)) t;
  RETURN QUERY SELECT mode, runs_in_scan('EXECUTE by_day(''2026-02-01'')'), diff = 0 AND (SELECT count(*) FROM r_on) = 7;
END $$;
SELECT * FROM check_prepared('force_custom_plan');
SELECT * FROM check_prepared('force_generic_plan');
-- a full index next to the partial one: either answers a query the partial one covers
CREATE INDEX facts_col_all ON facts USING sift_col (id, grp, day, deleted_at, score, doc) WITH (paths = 'doc.k int4');
VACUUM (FREEZE, ANALYZE) facts;
SELECT * FROM check_all();
DROP INDEX facts_col_all;
-- a partitioned table: each partition holds its own partial index
CREATE TABLE ev (id int, grp int, day date, deleted_at timestamp, score float8) PARTITION BY RANGE (day);
CREATE TABLE ev_a PARTITION OF ev FOR VALUES FROM ('2026-01-01') TO ('2026-02-01');
CREATE TABLE ev_b PARTITION OF ev FOR VALUES FROM ('2026-02-01') TO ('2026-04-01');
INSERT INTO ev SELECT i, i % 7, date '2026-01-01' + i % 80, CASE WHEN i % 10 = 0 THEN timestamp '2026-03-01' END, i % 13 FROM generate_series(1, 20000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, grp, day, deleted_at, score) WHERE deleted_at IS NULL;
VACUUM (FREEZE, ANALYZE) ev;
SELECT * FROM check_query('partitions, implied', 'SELECT grp, count(*), sum(score) FROM ev WHERE deleted_at IS NULL AND day >= ''2026-01-20'' GROUP BY grp');
SELECT * FROM check_query('partitions, not implied', 'SELECT grp, count(*), sum(score) FROM ev WHERE day >= ''2026-01-20'' GROUP BY grp');
UPDATE ev SET deleted_at = timestamp '2026-03-05' WHERE id % 40 = 1;
UPDATE ev SET deleted_at = NULL WHERE id % 40 = 0;
SELECT * FROM check_query('partitions, after changes', 'SELECT grp, count(*), sum(score) FROM ev WHERE deleted_at IS NULL AND day >= ''2026-01-20'' GROUP BY grp');
DROP FUNCTION check_prepared(text), check_all(), check_query(text, text), outcome(text, bool), runs_in_scan(text), pushed, runq;
DROP TABLE facts, dims, ev;
DROP EXTENSION pg_sift;
