-- The scans of PostgreSQL are off, as in the plan-choice gate, so that the node plans where it can. A dimension that no column of the fact table equals (an inequality, BETWEEN, OR, IS NOT DISTINCT FROM,
-- ON true, an expression of columns), a condition of the ON clause of an outer join that no key says, a WHERE over both sides of a RIGHT or FULL join, a dimension that is a function, a
-- FULL join of a second dimension, and a group key or argument computed from columns beside a semi or anti join: the rows must be those of sift.enable_agg = off, with NULL keys on both sides,
-- keys that match many rows or none, dimension rows no fact row matches, fact rows no dimension row matches, and more fact rows than one batch holds.
CREATE EXTENSION pg_sift;
DO $$ BEGIN
	IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF;
END $$;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET enable_seqscan = off;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SET enable_indexonlyscan = off;

CREATE TABLE f (id int PRIMARY KEY, k int, k2 int, g text, v int) WITH (autovacuum_enabled = false);
INSERT INTO f SELECT i, CASE WHEN i % 13 = 0 THEN NULL WHEN i % 7 = 0 THEN 900 + i % 3 ELSE i % 40 END, CASE WHEN i % 11 = 0 THEN NULL ELSE i % 25 END, CASE WHEN i % 17 = 0 THEN NULL ELSE 'g' || (i % 5) END, i % 50 FROM generate_series(1, 6000) i;
CREATE INDEX f_col ON f USING sift_col (id, k, k2, g, v);
VACUUM (FREEZE, ANALYZE) f;

-- keys 0..39 once, 10 of them again, a NULL key, a key no fact row has; every row is in a group of d2 or in none
CREATE TABLE d2 (id int PRIMARY KEY, name text) WITH (autovacuum_enabled = false);
INSERT INTO d2 SELECT i, 'n' || i % 3 FROM generate_series(1, 8) i;
CREATE TABLE d (id int PRIMARY KEY, k int, t text, w int, grp int) WITH (autovacuum_enabled = false);
INSERT INTO d SELECT i, i % 40, 'G' || (i % 5), i % 7, CASE WHEN i % 9 = 0 THEN NULL ELSE 1 + i % 10 END FROM generate_series(1, 50) i;
INSERT INTO d VALUES (51, NULL, 'G1', 3, 2), (52, 5000, 'G9', 8, 3);
VACUUM (FREEZE, ANALYZE) d, d2;

CREATE FUNCTION same(q text) RETURNS TABLE (engine text, result_rows bigint, vs_heap bigint) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	engine := 'normal';
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAggV6%' THEN engine := 'SiftAggV6'; END IF;
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS SELECT x::text AS r FROM (%s) x', q);
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS SELECT x::text AS r FROM (%s) x', q);
	RESET sift.enable_agg;
	result_rows := (SELECT count(*) FROM r_on);
	vs_heap := (SELECT count(*) FROM ((TABLE r_on EXCEPT ALL TABLE r_off) UNION ALL (TABLE r_off EXCEPT ALL TABLE r_on)) x);
	DROP TABLE r_on, r_off;
	RETURN NEXT;
END $$;

CREATE TABLE stmts (id int, q text);
INSERT INTO stmts VALUES
-- inner: the pairs of the rows, whichever condition keeps them
(1, 'SELECT d.t, count(*), sum(f.v) FROM f JOIN d ON d.k < f.k GROUP BY d.t'),
(2, 'SELECT d.t, count(*), sum(f.v) FROM f JOIN d ON d.k BETWEEN f.k AND f.k + 1 GROUP BY d.t'),
(3, 'SELECT d.t, count(*), sum(f.v) FROM f JOIN d ON d.k = f.k OR d.k = f.k2 GROUP BY d.t'),
(4, 'SELECT d.t, count(*), sum(f.v) FROM f JOIN d ON d.k IS NOT DISTINCT FROM f.k GROUP BY d.t'),
(5, 'SELECT d.t, count(*) FROM f JOIN d ON true GROUP BY d.t'),
(6, 'SELECT d.t, count(*), sum(f.v) FROM f JOIN d ON d.k = f.k + 1 GROUP BY d.t'),
(7, 'SELECT d.t, count(*), sum(f.v) FROM f JOIN d ON lower(d.t) = lower(f.g) GROUP BY d.t'),
(8, 'SELECT d.t, count(*), sum(f.v) FROM f JOIN d ON d.k = f.k2 % 7 GROUP BY d.t'),
(9, 'SELECT d.t, count(*) FROM f JOIN d ON d.k = f.k AND d.w < f.v % 7 GROUP BY d.t'),
(10, 'SELECT d.t, d2.name, count(*) FROM f JOIN d ON d.k = f.k JOIN d2 ON d2.id < d.grp GROUP BY d.t, d2.name'),
-- LEFT: a fact row no pair keeps stays with NULL dimension columns
(11, 'SELECT d.t, count(*), count(d.id), sum(f.v) FROM f LEFT JOIN d ON d.k = f.k AND f.v < 30 GROUP BY d.t'),
(12, 'SELECT d.t, count(*), count(d.id) FROM f LEFT JOIN d ON d.k < f.k GROUP BY d.t'),
(13, 'SELECT d.t, count(*), count(d.id) FROM f LEFT JOIN d ON d.k = f.k OR d.k = f.k2 GROUP BY d.t'),
(14, 'SELECT d.t, count(*), count(d.id) FROM f LEFT JOIN d ON d.k IS NOT DISTINCT FROM f.k GROUP BY d.t'),
(15, 'SELECT d.t, count(*), count(d.id) FROM f LEFT JOIN d ON d.k BETWEEN f.k AND f.k + 1 GROUP BY d.t'),
(16, 'SELECT d.t, count(*), count(d.id) FROM f LEFT JOIN d ON lower(d.t) = lower(f.g) GROUP BY d.t'),
(17, 'SELECT d.t, count(*), count(d.id) FROM f LEFT JOIN d ON d.k = f.k + 1 GROUP BY d.t'),
(18, 'SELECT d.t, count(*), count(d.id) FROM f LEFT JOIN d ON d.k = f.k AND d.w < f.v % 7 GROUP BY d.t'),
(19, 'SELECT d.t, count(*), count(d.id) FROM f LEFT JOIN d ON false GROUP BY d.t'),
-- RIGHT and FULL: a dimension row no pair keeps is unmatched
(20, 'SELECT d.t, count(f.id), count(*), sum(f.v) FROM f RIGHT JOIN d ON d.k = f.k AND d.w > 3 GROUP BY d.t'),
(21, 'SELECT d.t, count(f.id), count(*) FROM f RIGHT JOIN d ON d.k = f.k AND f.v < 30 GROUP BY d.t'),
(22, 'SELECT d.t, count(f.id), count(*) FROM f RIGHT JOIN d ON d.k < f.k GROUP BY d.t'),
(23, 'SELECT d.t, count(f.id), count(*) FROM f RIGHT JOIN d ON d.k = f.k OR d.k = f.k2 GROUP BY d.t'),
(24, 'SELECT d.t, count(f.id), count(*) FROM f RIGHT JOIN d ON d.k = f.k + 1 GROUP BY d.t'),
(25, 'SELECT d.t, count(f.id), count(*) FROM f FULL JOIN d ON d.k = f.k AND d.w > 3 GROUP BY d.t'),
(26, 'SELECT d.t, count(f.id), count(*) FROM f FULL JOIN d ON d.k = f.k AND f.v < 30 GROUP BY d.t'),
(27, 'SELECT d.t, count(f.id), count(*) FROM f FULL JOIN d ON d.k = f.k + 1 GROUP BY d.t'),
(28, 'SELECT d.t, count(f.id), count(*) FROM f FULL JOIN d ON lower(d.t) = lower(f.g) GROUP BY d.t'),
(29, 'SELECT d.t, count(f.id), count(*) FROM f FULL JOIN d ON d.k = f.k % 40 GROUP BY d.t'),
(30, 'SELECT d.t, count(f.id), count(*) FROM f FULL JOIN d ON d.k = f.k AND d.w < f.v % 7 GROUP BY d.t'),
-- a WHERE over both sides runs on the rows the join completed, the unmatched ones too
(31, 'SELECT d.t, count(f.id), count(*) FROM f RIGHT JOIN d ON d.k = f.k WHERE d.k IS NULL OR f.v < 30 GROUP BY d.t'),
(32, 'SELECT d.t, count(*) FROM f RIGHT JOIN d ON d.k = f.k WHERE f.id IS NULL GROUP BY d.t'),
(33, 'SELECT d.t, count(f.id), count(*) FROM f FULL JOIN d ON d.k = f.k WHERE d.id IS NULL GROUP BY d.t'),
(34, 'SELECT d.t, count(f.id), count(*) FROM f FULL JOIN d ON d.k = f.k WHERE d.id IS NULL OR f.v < 30 GROUP BY d.t'),
(35, 'SELECT d.t, count(f.id), count(*) FROM f FULL JOIN d ON d.k = f.k WHERE f.id IS NULL GROUP BY d.t'),
-- a FULL join of a second dimension, the last join of the statement
(36, 'SELECT d2.name, count(*), count(d.id) FROM f JOIN d ON d.k = f.k FULL JOIN d2 ON d2.id = d.grp GROUP BY d2.name'),
(37, 'SELECT d2.name, d.t, count(*), count(f.id) FROM f LEFT JOIN d ON d.k = f.k FULL JOIN d2 ON d2.id = d.grp AND d2.id < 6 GROUP BY d2.name, d.t'),
-- a dimension that is a function of the statement
(38, 'SELECT d.kind, count(*) FROM f JOIN unnest(ARRAY[1, 2, 3, 3]) WITH ORDINALITY AS d(id, kind) ON d.id = f.k GROUP BY d.kind'),
(39, 'SELECT d.kind, count(f.id), count(*) FROM f RIGHT JOIN unnest(ARRAY[1, 2, 3, 3, NULL]) WITH ORDINALITY AS d(id, kind) ON d.id = f.k GROUP BY d.kind'),
(40, 'SELECT d.kind, count(f.id), count(*) FROM f FULL JOIN unnest(ARRAY[1, 2, 3, 3, NULL, 7000]) WITH ORDINALITY AS d(id, kind) ON d.id = f.k GROUP BY d.kind'),
(41, 'SELECT g.g, count(*) FROM f JOIN generate_series(0, 9) AS g(g) ON g.g = f.k GROUP BY g.g'),
(42, 'SELECT g.g, count(*) FROM f LEFT JOIN generate_series(0, 9) AS g(g) ON g.g < f.k AND f.v = 3 GROUP BY g.g'),
-- a group key or an argument the scan computes from columns, beside a join
(43, 'SELECT f.k % 5, count(*) FROM f WHERE NOT EXISTS (SELECT 1 FROM d WHERE d.k = f.k) GROUP BY 1'),
(44, 'SELECT f.k % 5, count(*) FROM f WHERE EXISTS (SELECT 1 FROM d WHERE d.k = f.k) GROUP BY 1'),
(45, 'SELECT d.t, sum(f.v * 2), sum(f.k % 7), count(*) FROM f JOIN d ON d.k = f.k GROUP BY d.t'),
(46, 'SELECT f.k2 % 3 AS a, d.t, count(*), sum(f.v + f.k2) FROM f LEFT JOIN d ON d.k = f.k GROUP BY 1, 2'),
(47, 'SELECT length(f.g) AS n, d.t, count(*) FROM f JOIN d ON d.k = f.k GROUP BY 1, 2'),
-- a numeric the condition computes from a fact column, which PostgreSQL's evaluator takes
(49, 'SELECT d.t, count(*) FROM f JOIN d ON d.k = f.k AND d.w > f.v::numeric / 10 GROUP BY d.t'),
-- the node leaves these to PostgreSQL: an exact numeric product beside a join, a condition of a semi join that no key says (it would decide on the first match only)
(50, 'SELECT d.t, sum(f.v::numeric * 2.5) FROM f JOIN d ON d.k = f.k GROUP BY d.t'),
(51, 'SELECT count(*) FROM f WHERE EXISTS (SELECT 1 FROM d WHERE d.k = f.k AND d.w < f.v % 7)');

SELECT id, (same(q)).* FROM stmts ORDER BY id;

-- a dimension the transaction changed, and an empty one
BEGIN;
DELETE FROM d WHERE k < 5;
INSERT INTO d VALUES (60, 3, 'G1', 1, 1), (61, NULL, 'G2', 2, NULL);
SELECT id, (same(q)).* FROM stmts WHERE id IN (1, 6, 11, 22, 26, 30, 32, 35, 36) ORDER BY id;
ROLLBACK;
BEGIN;
DELETE FROM d;
SELECT id, (same(q)).* FROM stmts WHERE id IN (1, 5, 11, 20, 25, 33, 36) ORDER BY id;
ROLLBACK;

DROP FUNCTION same(text);
DROP TABLE stmts, f, d, d2;
DROP EXTENSION pg_sift;
