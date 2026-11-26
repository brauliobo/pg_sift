-- A sift_col index that finds its own damage leaves service: the statement that finds it fails once, the next ones are planned on the heap and give the rows the index gave,
-- and REINDEX puts the index back. sift.on_corruption = error does not bring the index back.
CREATE EXTENSION pg_sift_test CASCADE;
CREATE EXTENSION pg_sift_helpers;
CREATE FUNCTION plan_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	r text;
	p text := '';
BEGIN
	FOR r IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		p := p || r || E'\n';
	END LOOP;
	RETURN CASE WHEN p ~ 'Sift|t_idx' THEN 'sift' ELSE 'heap' END;
END $$;

SET client_min_messages = warning;
SET sift.on_corruption = quarantine;
CREATE TABLE t (id int, v int, g int);
INSERT INTO t SELECT i, i % 7, i % 3 FROM generate_series(1, 20000) i;
CREATE INDEX t_idx ON t USING sift_col (id, v, g);
ANALYZE t;
CREATE TABLE answers AS
	SELECT 'count' AS q, (SELECT count(*) FROM t WHERE v = 3)::text AS a
	UNION ALL SELECT 'sum', (SELECT sum(id) FROM t WHERE g = 1)::text
	UNION ALL SELECT 'groups', (SELECT string_agg(v || ':' || n, ',' ORDER BY v) FROM (SELECT v, count(*) n FROM t GROUP BY v) s);
SELECT plan_of('SELECT count(*) FROM t WHERE v = 3') AS plan_before;

CREATE TABLE healthy (id int, v int);
CREATE INDEX healthy_idx ON healthy USING sift_col (id, v) WITH (track_changes = ctid);
INSERT INTO healthy SELECT i, i % 7 FROM generate_series(1, 3000) i;
UPDATE healthy SET v = v + 1 WHERE id <= 100;
DELETE FROM healthy WHERE id > 2900;
SELECT count(*) AS rows_written, (SELECT count(*) FROM sift.check('healthy_idx')) AS problems FROM healthy;
DROP TABLE healthy;

SET plan_cache_mode = force_generic_plan;
PREPARE by_v AS SELECT count(*) FROM t WHERE v = 3;
EXECUTE by_v;

SELECT sift._test_poke('t_idx', min(first_block), 8176, 9, 2) FROM sift.col_extents('t_idx') WHERE what IN ('segment extent', 'part');
SELECT sift_helpers.statement_outcome($$ SELECT count(*) FROM t WHERE v = 3 $$, ': ') AS detecting_statement;
SELECT index, error_code, detections FROM sift.quarantine;
SELECT plan_of('SELECT count(*) FROM t WHERE v = 3') AS plan_after;
SELECT 'count' AS q, (SELECT count(*) FROM t WHERE v = 3)::text AS a
UNION ALL SELECT 'sum', (SELECT sum(id) FROM t WHERE g = 1)::text
UNION ALL SELECT 'groups', (SELECT string_agg(v || ':' || n, ',' ORDER BY v) FROM (SELECT v, count(*) n FROM t GROUP BY v) s)
EXCEPT SELECT q, a FROM answers;
SELECT sift_helpers.statement_outcome($$ SELECT count(*) FROM t WHERE v = 3 $$, ': ') AS next_statement;
-- the plan made before the index left service is refused once, and the retry is planned without it
-- the worker that flags the metapage invalidates the plan too: a plan that it did not reach is refused once, and one that it did is planned again
SELECT sift_helpers.statement_outcome($$ EXECUTE by_v $$, ': ') IN ('ok', '40001: sift index "t_idx" is out of service; the statement must be planned again') AS stale_plan_refused_or_replanned;
SELECT sift_helpers.statement_outcome($$ EXECUTE by_v $$, ': ') AS replanned;
EXECUTE by_v;
SELECT sift_helpers.statement_outcome($$ SELECT * FROM t WHERE id = 5 $$, ': ') AS point_lookup;

-- the error mode of a session does not bring the quarantined index back: only REINDEX does
SET sift.on_corruption = error;
SELECT plan_of('SELECT count(*) FROM t WHERE v = 3') AS plan_in_error_mode;
SET sift.on_corruption = quarantine;

REINDEX INDEX t_idx;
SELECT count(*) AS listed_after_reindex FROM sift.quarantine;
SELECT plan_of('SELECT count(*) FROM t WHERE v = 3') AS plan_after_reindex;
SELECT plan_of('EXECUTE by_v') AS prepared_plan_after_reindex;
SELECT count(*) FROM t WHERE v = 3;

DEALLOCATE by_v;
DROP TABLE answers, t;
DROP FUNCTION plan_of(text);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
