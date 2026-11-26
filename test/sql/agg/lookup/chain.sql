-- A lookup join keyed by columns of an earlier lookup's table (entities of the same type, with NULLs): the rows are the same as the normal plan's.
-- SiftAggV6 keeps the normal plan for a second lookup keyed by the first (explain_why: where).
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;

CREATE TABLE facts (id int PRIMARY KEY, ent_id int, cat text, amount numeric(10, 2)) WITH (autovacuum_enabled = false);
INSERT INTO facts
SELECT i, CASE WHEN i % 97 = 0 THEN NULL WHEN i % 89 = 0 THEN 900 ELSE 1 + abs(hashint4(i)) % 530 END, 'c' || abs(hashint4(i + 7)) % 6, (i % 100) / 4.0
FROM generate_series(1, 20000) i;
CREATE INDEX facts_col ON facts USING sift_col (ent_id, cat, amount);
VACUUM (FREEZE, ANALYZE) facts;
CREATE TABLE ents (id int PRIMARY KEY, kind text, tier int, score float8) WITH (autovacuum_enabled = false);
INSERT INTO ents
SELECT i, CASE WHEN i % 11 = 0 THEN NULL ELSE 'k' || i % 4 END, CASE WHEN i % 13 = 0 THEN NULL ELSE i % 3 END, (i % 5) / 2.0
FROM generate_series(1, 500) i;
VACUUM (FREEZE, ANALYZE) ents;
CREATE TABLE maps (kind text, tier int, cat text, label text) WITH (autovacuum_enabled = false);
INSERT INTO maps
SELECT 'k' || k, t, 'c' || c, 'L' || (k + t + c) % 7
FROM generate_series(0, 3) k, generate_series(0, 2) t, generate_series(0, 5) c
WHERE (k + t * 2 + c) % 4 <> 0;
VACUUM (FREEZE, ANALYZE) maps;

-- the second lookup is keyed by a text and an integer column of the first, the entity of some facts is missing or NULL
\set chain_label 'SELECT coalesce(m.label, ''none'') AS label, count(*) AS n, sum(f.amount) AS total FROM facts f LEFT JOIN ents e ON e.id = f.ent_id LEFT JOIN maps m ON m.kind = e.kind AND m.tier = e.tier AND m.cat = f.cat GROUP BY 1'
-- an expression over columns of both lookups, a float among them
\set chain_expression 'SELECT coalesce(m.label, ''none'') || coalesce(e.kind, ''?'') || coalesce(e.tier::text, ''?'') AS ty, e.score * 2 AS s2, count(*) AS n FROM facts f LEFT JOIN ents e ON e.id = f.ent_id LEFT JOIN maps m ON m.kind = e.kind AND m.tier = e.tier AND m.cat = f.cat GROUP BY 1, 2'

CREATE FUNCTION explain_lines(q text) RETURNS SETOF text LANGUAGE plpgsql AS $$ BEGIN RETURN QUERY EXECUTE 'EXPLAIN (COSTS OFF) ' || q; END $$;
EXPLAIN (COSTS OFF) :chain_label;
EXPLAIN (COSTS OFF) :chain_expression;
CREATE TEMP TABLE r1_on AS :chain_label;
CREATE TEMP TABLE r2_on AS :chain_expression;
SET sift.enable_agg = off;
CREATE TEMP TABLE r1_off AS :chain_label;
CREATE TEMP TABLE r2_off AS :chain_expression;
RESET sift.enable_agg;
SELECT (SELECT count(*) FROM r1_on) AS labels, (SELECT count(*) FROM ((TABLE r1_on EXCEPT TABLE r1_off) UNION ALL (TABLE r1_off EXCEPT TABLE r1_on)) d) AS differences;
SELECT (SELECT count(*) FROM r2_on) AS types, (SELECT count(*) FROM ((TABLE r2_on EXCEPT TABLE r2_off) UNION ALL (TABLE r2_off EXCEPT TABLE r2_on)) d) AS differences;
SELECT label, n FROM r1_on WHERE label IN ('none', 'L0') ORDER BY label;

-- the same chain of LEFT joins over plain columns: the second join's ON clause reads the first's table, so PostgreSQL may commute them (and clones the clause for each
-- order); the scan joins in the order of the statement and answers it
\set chain_columns 'SELECT m.label, e.kind, count(*) AS n, sum(f.amount) AS total, count(m.label) AS labelled FROM facts f LEFT JOIN ents e ON e.id = f.ent_id LEFT JOIN maps m ON m.kind = e.kind AND m.tier = e.tier AND m.cat = f.cat GROUP BY 1, 2'
\set chain_where 'SELECT e.tier, count(*) AS n FROM facts f LEFT JOIN ents e ON e.id = f.ent_id LEFT JOIN maps m ON m.kind = e.kind AND m.tier = e.tier WHERE m.label IS NULL GROUP BY 1'
SELECT count(*) FILTER (WHERE l LIKE '%Custom Scan (SiftAggV6)%') AS v6 FROM (SELECT * FROM explain_lines(:'chain_columns') UNION ALL SELECT * FROM explain_lines(:'chain_where')) x(l);
CREATE TEMP TABLE r3_on AS :chain_columns;
CREATE TEMP TABLE r4_on AS :chain_where;
SET sift.enable_agg = off;
CREATE TEMP TABLE r3_off AS :chain_columns;
CREATE TEMP TABLE r4_off AS :chain_where;
RESET sift.enable_agg;
SELECT (SELECT count(*) FROM ((TABLE r3_on EXCEPT ALL TABLE r3_off) UNION ALL (TABLE r3_off EXCEPT ALL TABLE r3_on)) d) AS columns_differences,
       (SELECT count(*) FROM ((TABLE r4_on EXCEPT ALL TABLE r4_off) UNION ALL (TABLE r4_off EXCEPT ALL TABLE r4_on)) d) AS where_differences;
DROP FUNCTION explain_lines(text);
DROP TABLE facts, ents, maps;
DROP EXTENSION pg_sift;
