-- Queries and DML of ordinary roles on tables with the three kinds of sift objects: what each privilege level may read and write, the same
-- answers through the index and the plain plan, row-level security, column privileges, views and SECURITY DEFINER functions, prepared
-- statements, cursors, COPY and EXPLAIN. Every role has USAGE on the schema sift except rq_nos.
-- docs (sift and sift_col indexes) and facts (sift_table) have no row-level security; rdocs and rfacts are the same with a policy that
-- restricts rq_rls to the rows of g = 1.
CREATE EXTENSION pg_sift;
\set VERBOSITY terse
\set ECHO none
\ir test/roles_helpers.sql
\set ECHO all
SELECT rr_roles_drop('rq_');
CREATE ROLE rq_own;
CREATE ROLE rq_mem IN ROLE rq_own;
CREATE ROLE rq_wr;
CREATE ROLE rq_rd;
CREATE ROLE rq_col;
CREATE ROLE rq_non;
CREATE ROLE rq_nos;
CREATE ROLE rq_rls;
CREATE ROLE rq_byp BYPASSRLS;
CREATE ROLE rq_vw;
CREATE SCHEMA rq;
GRANT USAGE ON SCHEMA rq TO rq_wr, rq_rd, rq_col, rq_non, rq_nos, rq_rls, rq_byp, rq_vw;
GRANT USAGE, CREATE ON SCHEMA rq TO rq_own, rq_mem;
GRANT USAGE ON SCHEMA sift TO rq_own, rq_mem, rq_wr, rq_rd, rq_col, rq_non, rq_rls, rq_byp, rq_vw;
SET enable_seqscan = off;

SELECT * FROM rr_run($$
rq_own | CREATE TABLE rq.docs (id int PRIMARY KEY, body text, g int, v float8)
rq_own | INSERT INTO rq.docs SELECT i, 'alpha ' || CASE WHEN i % 5 = 0 THEN 'five ' ELSE '' END || 'beta ' || i, i % 3, i FROM generate_series(1, 300) i
rq_own | CREATE INDEX docs_idx ON rq.docs USING sift (body)
rq_own | CREATE INDEX docs_col ON rq.docs USING sift_col (id, g, v)
rq_own | CREATE TABLE rq.facts (id int, g int, val float8, doc jsonb) USING sift_table
rq_own | INSERT INTO rq.facts SELECT i, i % 3, i, jsonb_build_object('a', i % 7) FROM generate_series(1, 300) i
rq_own | CREATE TABLE rq.rdocs AS SELECT * FROM rq.docs
rq_own | CREATE INDEX rdocs_idx ON rq.rdocs USING sift (body)
rq_own | CREATE INDEX rdocs_col ON rq.rdocs USING sift_col (id, g, v)
rq_own | CREATE TABLE rq.rfacts (LIKE rq.facts) USING sift_table
rq_own | INSERT INTO rq.rfacts SELECT * FROM rq.facts
rq_own | GRANT SELECT ON rq.docs, rq.facts, rq.rdocs, rq.rfacts TO rq_rd, rq_nos, rq_rls, rq_byp
rq_own | GRANT SELECT, INSERT, UPDATE, DELETE ON rq.docs, rq.facts, rq.rdocs, rq.rfacts TO rq_wr
rq_own | GRANT SELECT (id, g) ON rq.docs TO rq_col
rq_own | CREATE VIEW rq.docs_v AS SELECT id, body, g, v FROM rq.docs
rq_own | GRANT SELECT ON rq.docs_v TO rq_vw
rq_own | CREATE FUNCTION rq.count_docs(q text) RETURNS bigint LANGUAGE sql SECURITY DEFINER SET search_path = pg_catalog AS 'SELECT count(*) FROM rq.docs WHERE body @@> q'
rq_own | GRANT EXECUTE ON FUNCTION rq.count_docs(text) TO rq_non
rq_own | ALTER TABLE rq.rdocs ENABLE ROW LEVEL SECURITY
rq_own | ALTER TABLE rq.rfacts ENABLE ROW LEVEL SECURITY
rq_own | CREATE POLICY rdocs_all ON rq.rdocs FOR ALL TO PUBLIC USING (true)
rq_own | CREATE POLICY rdocs_rls ON rq.rdocs AS RESTRICTIVE FOR SELECT TO rq_rls USING (g = 1)
rq_own | CREATE POLICY rfacts_all ON rq.rfacts FOR ALL TO PUBLIC USING (true)
rq_own | CREATE POLICY rfacts_rls ON rq.rfacts AS RESTRICTIVE FOR SELECT TO rq_rls USING (g = 1)
$$);
ANALYZE rq.docs, rq.facts, rq.rdocs, rq.rfacts;

-- the BM25 index: the operator needs no privilege on the sift schema, the ranking function needs USAGE on it; the column privileges must cover
-- the indexed column; row-level security cuts the rows of the policy and refuses the score
SELECT * FROM rr_matrix(ARRAY['rq_own', 'rq_mem', 'rq_wr', 'rq_rd', 'rq_nos', 'rq_col', 'rq_non', 'rq_vw'], $$
SELECT count(*) FROM rq.docs WHERE body @@> 'alpha'
SELECT count(*) FROM rq.docs WHERE body @@> 'five' AND g = 1
SELECT id, round(sift.score(ctid)::numeric, 3) FROM rq.docs WHERE body @@> 'alpha five' ORDER BY 2 DESC, 1 LIMIT 3
SELECT count(*) FROM rq.docs_v WHERE body @@> 'alpha'
SELECT rq.count_docs('five')
EXPLAIN SELECT id FROM rq.docs WHERE body @@> 'five'
$$);
SELECT * FROM rr_matrix(ARRAY['rq_own', 'rq_byp', 'rq_rd', 'rq_rls'], $$
SELECT count(*) FROM rq.rdocs WHERE body @@> 'alpha'
SELECT count(*) FROM rq.rdocs WHERE body @@> 'five'
SELECT id, round(sift.score(ctid)::numeric, 3) FROM rq.rdocs WHERE body @@> 'alpha five' ORDER BY 2 DESC, 1 LIMIT 3
EXPLAIN SELECT id FROM rq.rdocs WHERE body @@> 'five'
$$);

-- the aggregate pushdown on the sift_col index and on the sift_table: the answers of the plain plan for every role, the same columns refused.
-- Known gaps (3.2.0 on, not fixed here): rq_col, who may read the columns id and g only, and rq_vw, who may read only a view of the table, are
-- refused "permission denied for table" at the run of a pushed-down plan that the plain plan answers (the check of the scan wants SELECT on the
-- whole table for the invoking role); the expected output below records it, and the fix changes it to the rows of the plain plan.
SET sift.enable_agg = on;
SET seq_page_cost = 10000;
SELECT * FROM rr_matrix(ARRAY['rq_own', 'rq_mem', 'rq_wr', 'rq_rd', 'rq_col', 'rq_non', 'rq_vw'], $$
SELECT g, count(*), sum(v) FROM rq.docs GROUP BY g ORDER BY g
SELECT g, count(*) FROM rq.docs GROUP BY g ORDER BY g
EXPLAIN SELECT g, count(*), sum(v) FROM rq.docs GROUP BY g
EXPLAIN SELECT g, count(*) FROM rq.docs GROUP BY g
SELECT g, count(*), sum(val) FROM rq.facts GROUP BY g ORDER BY g
EXPLAIN SELECT g, count(*), sum(val) FROM rq.facts GROUP BY g
$$);
SELECT * FROM rr_matrix(ARRAY['rq_own', 'rq_byp', 'rq_rd', 'rq_rls'], $$
SELECT g, count(*), sum(v) FROM rq.rdocs GROUP BY g ORDER BY g
EXPLAIN SELECT g, count(*), sum(v) FROM rq.rdocs GROUP BY g
SELECT g, count(*), sum(val) FROM rq.rfacts GROUP BY g ORDER BY g
EXPLAIN SELECT g, count(*), sum(val) FROM rq.rfacts GROUP BY g
$$);
-- the same aggregate over a view: a role that may read the view and not the table gets the answer of the plain plan
SELECT * FROM rr_matrix(ARRAY['rq_own', 'rq_vw'], $$
SELECT g, count(*), sum(v) FROM rq.docs_v GROUP BY g ORDER BY g
$$);
SET sift.enable_agg = off;
SELECT * FROM rr_matrix(ARRAY['rq_own', 'rq_vw'], $$
SELECT g, count(*), sum(v) FROM rq.docs_v GROUP BY g ORDER BY g
$$);
RESET sift.enable_agg;
RESET seq_page_cost;

-- DML: the reader and the column-only role are refused, the writer changes rows and the indexes follow; no TRUNCATE without the privilege
SELECT * FROM rr_run($$
rq_rd | INSERT INTO rq.docs VALUES (1000, 'zeta', 0, 0)
rq_rd | UPDATE rq.docs SET v = 0
rq_rd | DELETE FROM rq.docs
rq_rd | TRUNCATE rq.docs
rq_rd | MERGE INTO rq.docs d USING (VALUES (1)) s(id) ON d.id = s.id WHEN MATCHED THEN DELETE
rq_rd | INSERT INTO rq.facts VALUES (1000, 0, 0, '{}')
rq_col | UPDATE rq.docs SET g = 5
rq_col | DELETE FROM rq.docs
rq_non | INSERT INTO rq.docs VALUES (1000, 'zeta', 0, 0)
rq_wr | INSERT INTO rq.docs VALUES (1000, 'zeta gamma', 0, 1000)
rq_wr | INSERT INTO rq.docs SELECT i + 1000, 'zeta ' || i, i % 3, i FROM generate_series(1, 20) i
rq_wr | UPDATE rq.docs SET body = 'zeta moved' WHERE id = 1001
rq_wr | UPDATE rq.docs SET v = v + 1 WHERE g = 1 AND id > 1000
rq_wr | DELETE FROM rq.docs WHERE id > 1010
rq_wr | INSERT INTO rq.docs VALUES (1000, 'zeta again', 0, 0) ON CONFLICT (id) DO UPDATE SET body = excluded.body
rq_wr | MERGE INTO rq.docs d USING (VALUES (1, 'merged'), (5000, 'fresh')) s(id, b) ON d.id = s.id WHEN MATCHED THEN UPDATE SET body = s.b WHEN NOT MATCHED THEN INSERT (id, body, g, v) VALUES (s.id, s.b, 0, 0)
rq_wr | DELETE FROM rq.docs WHERE id = 5000 RETURNING id
rq_wr | SELECT count(*) FILTER (WHERE body @@> 'zeta'), count(*) FILTER (WHERE body @@> 'merged'), count(*) FILTER (WHERE body @@> 'moved') FROM rq.docs
rq_wr | TRUNCATE rq.docs
rq_wr | INSERT INTO rq.facts SELECT i + 1000, i % 3, i, '{}' FROM generate_series(1, 20) i
rq_wr | UPDATE rq.facts SET val = val + 1 WHERE id > 1000 AND g = 1
rq_wr | MERGE INTO rq.facts f USING (VALUES (1, 7), (6000, 8)) s(id, n) ON f.id = s.id WHEN MATCHED THEN UPDATE SET val = s.n WHEN NOT MATCHED THEN INSERT (id, g, val, doc) VALUES (s.id, 0, s.n, '{}')
rq_wr | DELETE FROM rq.facts WHERE id > 1010
rq_wr | SELECT count(*), sum(val) FROM rq.facts
rq_wr | TRUNCATE rq.facts
rq_own | GRANT TRUNCATE ON rq.facts TO rq_wr
rq_wr | TRUNCATE rq.facts
rq_wr | SELECT count(*) FROM rq.facts
$$);
INSERT INTO rq.facts SELECT i, i % 3, i, jsonb_build_object('a', i % 7) FROM generate_series(1, 300) i;

-- COPY in and out
SET SESSION AUTHORIZATION rq_wr;
COPY rq.docs (id, body, g, v) FROM STDIN;
2001	copy row one	0	1
2002	copy row two	1	2
\.
COPY rq.facts (id, g, val) FROM STDIN;
2001	0	1
2002	1	2
\.
COPY (SELECT id, body FROM rq.docs WHERE body @@> 'copy' ORDER BY id) TO STDOUT;
COPY (SELECT g, count(*) FROM rq.facts GROUP BY g ORDER BY g) TO STDOUT;
RESET SESSION AUTHORIZATION;
SET SESSION AUTHORIZATION rq_rd;
COPY (SELECT id, body FROM rq.docs WHERE body @@> 'copy' ORDER BY id) TO STDOUT;
COPY (SELECT id, body FROM rq.docs WHERE id BETWEEN 2 AND 4 ORDER BY id) TO STDOUT;
\copy rq.docs (id, body) from '/dev/null'
\copy rq.facts (id) from '/dev/null'
RESET SESSION AUTHORIZATION;
SET SESSION AUTHORIZATION rq_non;
COPY (SELECT id FROM rq.docs WHERE body @@> 'copy') TO STDOUT;
COPY rq.facts (id) TO STDOUT;
RESET SESSION AUTHORIZATION;
SET SESSION AUTHORIZATION rq_col;
COPY (SELECT id, g FROM rq.docs WHERE id <= 3 ORDER BY id) TO STDOUT (FORMAT csv, HEADER);
COPY (SELECT id, body FROM rq.docs WHERE id <= 3) TO STDOUT;
RESET SESSION AUTHORIZATION;

-- prepared statements are checked and planned for the role that executes them: a plan made for the owner, who sees every row, is not reused
-- for the role under a policy (one execution per plan type: custom for the first five, then generic)
SELECT * FROM rr_run($$
rq_own | PREPARE q_count(text) AS SELECT count(*) FROM rq.docs WHERE body @@> $1
rq_own | PREPARE q_rank(text) AS SELECT id, round(sift.score(ctid)::numeric, 3) FROM rq.docs WHERE body @@> $1 ORDER BY 2 DESC, 1 LIMIT 2
rq_own | PREPARE q_row(text) AS SELECT count(*) FROM rq.rdocs WHERE body @@> $1
rq_own | EXECUTE q_count('five')
rq_rd | EXECUTE q_count('five')
rq_rd | EXECUTE q_rank('alpha five')
rq_non | EXECUTE q_count('five')
rq_col | EXECUTE q_rank('five')
rq_nos | EXECUTE q_rank('alpha five')
rq_own | EXECUTE q_row('five')
rq_rls | EXECUTE q_row('five')
rq_own | EXECUTE q_row('five')
rq_rls | EXECUTE q_row('five')
rq_byp | EXECUTE q_row('five')
$$);
SET plan_cache_mode = force_generic_plan;
SELECT * FROM rr_run($$
rq_own | EXECUTE q_row('five')
rq_rls | EXECUTE q_row('five')
rq_own | EXECUTE q_row('five')
rq_rd | EXECUTE q_count('five')
rq_non | EXECUTE q_count('five')
rq_rd | EXECUTE q_rank('alpha five')
$$);
RESET plan_cache_mode;
DEALLOCATE ALL;

-- cursors: scrollable and held, opened by a role and read in the same transaction; a role without the privilege cannot open one
BEGIN;
SET LOCAL SESSION AUTHORIZATION rq_rd;
DECLARE c SCROLL CURSOR FOR SELECT id, round(sift.score(ctid)::numeric, 3) AS s FROM rq.docs WHERE body @@> 'alpha five' ORDER BY 2 DESC, 1;
FETCH 2 FROM c;
FETCH BACKWARD 1 FROM c;
MOVE LAST FROM c;
FETCH PRIOR FROM c;
CLOSE c;
DECLARE h CURSOR WITH HOLD FOR SELECT g, count(*), sum(v) FROM rq.docs GROUP BY g ORDER BY g;
COMMIT;
FETCH ALL FROM h;
CLOSE h;
BEGIN;
SET LOCAL SESSION AUTHORIZATION rq_non;
DECLARE c3 CURSOR FOR SELECT count(*) FROM rq.docs WHERE body @@> 'alpha';
ROLLBACK;
BEGIN;
SET LOCAL SESSION AUTHORIZATION rq_rls;
DECLARE c4 CURSOR FOR SELECT id, sift.score(ctid) FROM rq.rdocs WHERE body @@> 'alpha five' ORDER BY 2 DESC;
ROLLBACK;
BEGIN;
SET LOCAL SESSION AUTHORIZATION rq_rls;
DECLARE c5 CURSOR FOR SELECT count(*) FROM rq.rdocs WHERE body @@> 'alpha five';
FETCH c5;
ROLLBACK;

-- EXPLAIN and EXPLAIN ANALYZE: a plan is made and an analyzed statement is run with the privileges of the role, and one that is refused changes nothing
SET sift.enable_agg = on;
SET seq_page_cost = 10000;
SELECT * FROM rr_run($$
rq_rd | EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT id FROM rq.docs WHERE body @@> 'five'
rq_rd | EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT g, count(*) FROM rq.facts GROUP BY g
rq_rd | EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) UPDATE rq.facts SET val = 0
rq_rd | EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) DELETE FROM rq.docs
rq_non | EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT id FROM rq.docs WHERE body @@> 'five'
rq_wr | EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) UPDATE rq.facts SET val = val WHERE id = 1
rq_wr | EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) DELETE FROM rq.docs WHERE id = 2002
rq_own | SELECT count(*), sum(val) FROM rq.facts
rq_own | SELECT count(*) FROM rq.docs
$$);
RESET sift.enable_agg;
RESET seq_page_cost;

DROP SCHEMA rq CASCADE;
DROP FUNCTION rr_run(text), rr_matrix(text[], text), rr_roles_drop(text);
DROP OWNED BY rq_own, rq_mem, rq_wr, rq_rd, rq_col, rq_non, rq_nos, rq_rls, rq_byp, rq_vw;
DROP ROLE rq_own, rq_mem, rq_wr, rq_rd, rq_col, rq_non, rq_nos, rq_rls, rq_byp, rq_vw;
DROP EXTENSION pg_sift;
