-- The cost estimate of a sift_col index makes the estimate of the selectivity of its clauses once: it reads the one PostgreSQL caches in the RestrictInfo,
-- so an IN list of N elements costs N calls of the restriction estimator for the whole planning, however many index paths the joins build.
-- The operator sift.=== (int4 = float4, pg_sift_test) counts the calls of its estimator: scalararraysel makes one for each element each time it estimates the clause.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

CREATE TABLE sel_fact (id int, k int, v int, w text DEFAULT 'x') WITH (autovacuum_enabled = false);
INSERT INTO sel_fact SELECT i, i % 3000, i % 7 FROM generate_series(1, 30000) i;
CREATE TABLE sel_dim (id int PRIMARY KEY, n int);
INSERT INTO sel_dim SELECT i, i FROM generate_series(0, 6) i;
-- the point directory of k is what the estimate reads for an equality
CREATE INDEX sel_fact_col ON sel_fact USING sift_col (id, k, v) WITH (key_cut = k, point_columns = 'k', rollup_dims = 'off');
ANALYZE sel_fact;
ANALYZE sel_dim;
SET sift.enable_agg = off;

\o /dev/null
-- the list of 100 keys; `w` is not in the index, so the planner builds index paths on it
SELECT sift._test_eq_sel_calls(true) AS discard \gset
SELECT format('EXPLAIN (COSTS OFF) SELECT * FROM sel_fact WHERE k OPERATOR(sift.===) ANY (%L::float4[])', (SELECT array_agg(g) FROM generate_series(1, 100) g)) \gexec
\o
SELECT sift._test_eq_sel_calls(true) <= 100 AS one_estimate_of_the_list;
\o /dev/null

-- joins build index paths with their own clauses; the list is still estimated once
SELECT format('EXPLAIN (COSTS OFF) SELECT * FROM sel_fact f JOIN sel_dim d ON d.id = f.v WHERE f.k OPERATOR(sift.===) ANY (%L::float4[])', (SELECT array_agg(g) FROM generate_series(1, 100) g)) \gexec
\o
SELECT sift._test_eq_sel_calls(true) <= 100 AS one_estimate_of_the_list_joined;

DROP TABLE sel_fact, sel_dim;
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
