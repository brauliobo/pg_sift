-- A partial sift_col index that does not hold the columns of its predicate (CREATE INDEX .. (dim_a_id, kind, qty) WHERE status = 'open') answers a query whose
-- conditions imply the predicate: every row of the index passes it, so the scan does not test the conditions the predicate implies. A query that does not imply
-- the predicate, or that reads the predicate's column, keeps the normal plan. Results with the scan off and on are the same after every change of the heap.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE events (id int, dim_a_id int, kind int, status text, flag bool, qty float8);
INSERT INTO events
SELECT i, i % 40, i % 5, CASE WHEN i % 11 = 0 THEN NULL ELSE (ARRAY['new', 'open', 'held', 'closed'])[1 + i % 4] END, i % 3 = 0, i % 13
FROM generate_series(1, 20000) i;
CREATE TABLE kinds (id int, label text);
INSERT INTO kinds SELECT i, CASE WHEN i % 2 = 0 THEN 'a' ELSE 'b' END FROM generate_series(0, 4) i;
CREATE INDEX events_open ON events USING sift_col (dim_a_id, kind, qty) WHERE status = 'open';
CREATE INDEX events_null ON events USING sift_col (dim_a_id, kind, qty) WHERE status IS NULL;
CREATE INDEX events_held_flag ON events USING sift_col (dim_a_id, kind, qty) WHERE status = 'held' AND flag;
VACUUM (FREEZE, ANALYZE) events;
ANALYZE kinds;
SET app.status = 'open';
\ir test/agg_helpers.sql
CREATE FUNCTION check_query(name text, q text) RETURNS TABLE (label text, scan bool, same bool, outcome text) LANGUAGE plpgsql AS $$
BEGIN
  RETURN QUERY SELECT name, runs_in_scan(q), outcome(q, false) = outcome(q, true), left(outcome(q, true), 60);
END $$;
CREATE FUNCTION check_all() RETURNS TABLE (label text, scan bool, same bool, outcome text) LANGUAGE sql AS $$
 SELECT * FROM check_query('implied', 'SELECT kind, count(*), sum(qty) FROM events WHERE status = ''open'' AND dim_a_id < 20 GROUP BY kind')
 UNION ALL SELECT * FROM check_query('the predicate alone', 'SELECT count(*) FROM events WHERE status = ''open''')
 UNION ALL SELECT * FROM check_query('not null and equal', 'SELECT kind, count(*) FROM events WHERE status IS NOT NULL AND status = ''open'' GROUP BY kind')
 UNION ALL SELECT * FROM check_query('folded constant', 'SELECT kind, count(*) FROM events WHERE status = lower(''OPEN'') GROUP BY kind')
 UNION ALL SELECT * FROM check_query('list of one', 'SELECT kind, count(*) FROM events WHERE status IN (''open'') GROUP BY kind')
 UNION ALL SELECT * FROM check_query('and with a disjunction', 'SELECT kind, count(*) FROM events WHERE status = ''open'' AND (dim_a_id < 5 OR kind = 2) GROUP BY kind')
 UNION ALL SELECT * FROM check_query('no condition on the status', 'SELECT kind, count(*) FROM events WHERE dim_a_id < 20 GROUP BY kind')
 UNION ALL SELECT * FROM check_query('other value', 'SELECT kind, count(*) FROM events WHERE status = ''new'' GROUP BY kind')
 UNION ALL SELECT * FROM check_query('weaker', 'SELECT kind, count(*) FROM events WHERE status <> ''closed'' GROUP BY kind')
 UNION ALL SELECT * FROM check_query('list of two', 'SELECT kind, count(*) FROM events WHERE status IN (''open'', ''held'') GROUP BY kind')
 UNION ALL SELECT * FROM check_query('or', 'SELECT kind, count(*) FROM events WHERE status = ''open'' OR dim_a_id < 5 GROUP BY kind')
 UNION ALL SELECT * FROM check_query('stable operand', 'SELECT kind, count(*) FROM events WHERE status = current_setting(''app.status'') GROUP BY kind')
 UNION ALL SELECT * FROM check_query('null predicate', 'SELECT kind, count(*), sum(qty) FROM events WHERE status IS NULL AND dim_a_id < 20 GROUP BY kind')
 UNION ALL SELECT * FROM check_query('null against the value', 'SELECT kind, count(*) FROM events WHERE status = NULL GROUP BY kind')
 UNION ALL SELECT * FROM check_query('two conditions', 'SELECT kind, count(*) FROM events WHERE flag AND status = ''held'' AND dim_a_id < 30 GROUP BY kind')
 UNION ALL SELECT * FROM check_query('one of two conditions', 'SELECT kind, count(*) FROM events WHERE status = ''held'' GROUP BY kind')
 UNION ALL SELECT * FROM check_query('group by the status', 'SELECT status, count(*) FROM events WHERE status = ''open'' GROUP BY status')
 UNION ALL SELECT * FROM check_query('aggregate of the status', 'SELECT kind, count(status) FROM events WHERE status = ''open'' GROUP BY kind')
 UNION ALL SELECT * FROM check_query('join', 'SELECT k.label, count(*) FROM events e JOIN kinds k ON k.id = e.kind WHERE e.status = ''open'' GROUP BY k.label')
 UNION ALL SELECT * FROM check_query('left join', 'SELECT k.label, count(e.dim_a_id) FROM kinds k LEFT JOIN events e ON e.kind = k.id AND e.status = ''open'' GROUP BY k.label')
$$;
SELECT * FROM check_all();
-- rows move into and out of the predicates: new rows of every status, a status changed both ways, a column the index holds changed, deleted rows
INSERT INTO events SELECT 20000 + i, i % 40, i % 5, (ARRAY['new', 'open', 'held', 'closed'])[1 + i % 4], i % 2 = 0, i % 7 FROM generate_series(1, 4000) i;
INSERT INTO events SELECT 30000 + i, i % 40, i % 5, NULL, true, 1 FROM generate_series(1, 300) i;
UPDATE events SET status = 'open' WHERE id % 50 = 1 AND status IS DISTINCT FROM 'open';
UPDATE events SET status = 'closed' WHERE id % 50 = 2 AND status = 'open';
UPDATE events SET status = NULL WHERE id % 70 = 3;
UPDATE events SET qty = qty + 1, kind = (kind + 1) % 5 WHERE id % 9 = 4;
DELETE FROM events WHERE id % 111 = 5;
SELECT * FROM check_all();
BEGIN;
UPDATE events SET status = 'open' WHERE id % 20 = 7;
INSERT INTO events SELECT 40000 + i, 1, 1, 'open', true, 1 FROM generate_series(1, 500) i;
ROLLBACK;
SELECT * FROM check_all();
VACUUM events;
SELECT * FROM check_all();
REINDEX INDEX events_open;
SELECT * FROM check_all();
-- a prepared statement: a custom plan folds the parameter into a constant that implies the predicate, a generic plan cannot know it
CREATE FUNCTION check_prepared(mode text) RETURNS TABLE (label text, scan bool, same bool) LANGUAGE plpgsql AS $$
DECLARE diff bigint;
BEGIN
  PERFORM set_config('plan_cache_mode', mode, true);
  PERFORM set_config('sift.enable_agg', 'off', true);
  EXECUTE 'DEALLOCATE ALL';
  EXECUTE 'PREPARE by_status (text) AS SELECT kind, count(*) AS n FROM events WHERE status = $1 GROUP BY kind';
  EXECUTE 'CREATE TEMP TABLE r_off ON COMMIT DROP AS EXECUTE by_status(''open'')';
  PERFORM set_config('sift.enable_agg', 'on', true);
  PERFORM set_config(s, 'false', true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
  EXECUTE 'DEALLOCATE ALL';
  EXECUTE 'PREPARE by_status (text) AS SELECT kind, count(*) AS n FROM events WHERE status = $1 GROUP BY kind';
  EXECUTE 'CREATE TEMP TABLE r_on ON COMMIT DROP AS EXECUTE by_status(''open'')';
  SELECT count(*) INTO diff FROM ((TABLE r_off EXCEPT TABLE r_on) UNION ALL (TABLE r_on EXCEPT TABLE r_off)) t;
  RETURN QUERY SELECT mode, runs_in_scan('EXECUTE by_status(''open'')'), diff = 0 AND (SELECT count(*) FROM r_on) = 5;
END $$;
SELECT * FROM check_prepared('force_custom_plan');
SELECT * FROM check_prepared('force_generic_plan');
-- a full index beside the partial ones: either answers a query they cover
CREATE INDEX events_all ON events USING sift_col (dim_a_id, kind, qty, status);
VACUUM (FREEZE, ANALYZE) events;
SELECT * FROM check_all();
DROP INDEX events_all;
-- a partitioned table: a partial index on each partition, or on the parent
CREATE TABLE pev (id int, dim_a_id int, kind int, status text, qty float8) PARTITION BY RANGE (dim_a_id);
CREATE TABLE pev_a PARTITION OF pev FOR VALUES FROM (0) TO (20);
CREATE TABLE pev_b PARTITION OF pev FOR VALUES FROM (20) TO (40);
INSERT INTO pev SELECT i, i % 40, i % 5, (ARRAY['new', 'open', 'held', 'closed'])[1 + i % 4], i % 13 FROM generate_series(1, 20000) i;
CREATE INDEX pev_open ON pev USING sift_col (dim_a_id, kind, qty) WHERE status = 'open';
VACUUM (FREEZE, ANALYZE) pev;
SELECT * FROM check_query('partitions, implied', 'SELECT kind, count(*), sum(qty) FROM pev WHERE status = ''open'' AND dim_a_id < 30 GROUP BY kind');
SELECT * FROM check_query('partitions, not implied', 'SELECT kind, count(*), sum(qty) FROM pev WHERE dim_a_id < 30 GROUP BY kind');
UPDATE pev SET status = 'open' WHERE id % 40 = 1;
UPDATE pev SET status = 'new' WHERE id % 40 = 2 AND status = 'open';
SELECT * FROM check_query('partitions, after changes', 'SELECT kind, count(*), sum(qty) FROM pev WHERE status = ''open'' AND dim_a_id < 30 GROUP BY kind');
-- the partitions do not share a predicate: the plan of the table is the normal one
DROP INDEX pev_open;
CREATE INDEX pev_a_open ON pev_a USING sift_col (dim_a_id, kind, qty) WHERE status = 'open';
CREATE INDEX pev_b_held ON pev_b USING sift_col (dim_a_id, kind, qty) WHERE status = 'held';
VACUUM (FREEZE, ANALYZE) pev;
SELECT * FROM check_query('partitions, other predicates', 'SELECT kind, count(*) FROM pev WHERE status = ''open'' GROUP BY kind');
SELECT * FROM check_query('partitions, one pruned', 'SELECT kind, count(*) FROM pev WHERE status = ''open'' AND dim_a_id < 10 GROUP BY kind');
DROP FUNCTION check_prepared(text), check_all(), check_query(text, text), outcome(text, bool), runs_in_scan(text), pushed, runq;
DROP TABLE events, kinds, pev;
DROP EXTENSION pg_sift;
