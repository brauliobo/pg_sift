-- A lookup join keyed by columns of an earlier lookup's table makes one combo entry per distinct value of those columns, not per row of the
-- earlier table that holds them: entities of the same type, with NULLs, share an entry. The rows are the same as the normal plan's.
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

EXPLAIN (COSTS OFF) :chain_label;
EXPLAIN (COSTS OFF) :chain_expression;
CREATE TEMP TABLE r1_on AS :chain_label;
CREATE TEMP TABLE r2_on AS :chain_expression;
SET sift.enable_agg = off;
CREATE TEMP TABLE r1_off AS :chain_label;
CREATE TEMP TABLE r2_off AS :chain_expression;
RESET sift.enable_agg;
-- the caches start again before every chunk (the ids of the values go with the output rows they stand for)
SET sift.test_limits = 'combo_entries=0';
CREATE TEMP TABLE r1_trim AS :chain_label;
CREATE TEMP TABLE r2_trim AS :chain_expression;
RESET sift.test_limits;
SELECT (SELECT count(*) FROM r1_on) AS labels, (SELECT count(*) FROM ((TABLE r1_on EXCEPT TABLE r1_off) UNION ALL (TABLE r1_off EXCEPT TABLE r1_on)) d) AS differences;
SELECT (SELECT count(*) FROM r2_on) AS types, (SELECT count(*) FROM ((TABLE r2_on EXCEPT TABLE r2_off) UNION ALL (TABLE r2_off EXCEPT TABLE r2_on)) d) AS differences;
SELECT label, n FROM r1_on WHERE label IN ('none', 'L0') ORDER BY label;
SELECT (SELECT count(*) FROM ((TABLE r1_trim EXCEPT TABLE r1_off) UNION ALL (TABLE r1_off EXCEPT TABLE r1_trim)) d) AS differences_restarted_1,
       (SELECT count(*) FROM ((TABLE r2_trim EXCEPT TABLE r2_off) UNION ALL (TABLE r2_off EXCEPT TABLE r2_trim)) d) AS differences_restarted_2;
DROP TABLE facts, ents, maps;
DROP EXTENSION pg_sift;
