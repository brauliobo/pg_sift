-- The lists of dead rows that an OFFSET over the global order asks for (the rows a segment lost to VACUUM, by unit) are kept by the backend for the next scan: the second query finds them
-- (sift._test_col_gdead_stats), and a query after more rows were cleared (DELETE and VACUUM, an UPDATE, a promotion, a merge, new rows at the freed addresses) makes them again. Every answer is the
-- table's without the index (p takes the same writes and is the oracle), between two queries of the same backend, and through a script of random writes.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SELECT setseed(0.42);

CREATE TABLE t (id int PRIMARY KEY, k int, u int, pad text DEFAULT repeat('x', 100));
ALTER TABLE t ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO t (id, k, u) SELECT i, (hashint4(i) & 16777215) % 2000, (hashint4(i + 5) & 16777215) % 1000003 FROM generate_series(1, 30000) i;
CREATE INDEX t_ix ON t USING sift_col (id, k, u) WITH (order_array_columns = 'k, u', write_buffer_limit = 64);
CREATE TABLE p AS TABLE t;
ANALYZE t;
ANALYZE p;
SET sift.agg_force = on;

CREATE FUNCTION same(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	EXECUTE format('SELECT string_agg(r::text, '','') FROM (%s) r', replace(q, '@', 't')) INTO a;
	EXECUTE format('SELECT string_agg(r::text, '','') FROM (%s) r', replace(q, '@', 'p')) INTO b;
	RETURN a IS NOT DISTINCT FROM b;
END $$;
-- ties (k has 2,000 values: the group the offset ends in is fetched, the groups before it are counted), a key with no repeats, descending, a filtered and a deep one
CREATE FUNCTION allq() RETURNS boolean LANGUAGE sql AS $$
	SELECT same('SELECT id, k FROM @ ORDER BY k, id LIMIT 20 OFFSET 4000') AND same('SELECT id, u FROM @ ORDER BY u, id LIMIT 20 OFFSET 6000')
	   AND same('SELECT id, k FROM @ ORDER BY k DESC, id LIMIT 20 OFFSET 12000') AND same('SELECT id, u FROM @ WHERE k < 1000 ORDER BY u DESC, id LIMIT 20 OFFSET 3000')
	   AND same('SELECT id, k FROM @ ORDER BY k, id LIMIT 20 OFFSET 20000') $$;
CREATE TABLE res (step text, equal boolean, hits int8, builds int8);
CREATE FUNCTION step(name text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE h0 int8; b0 int8; h1 int8; b1 int8; ok boolean;
BEGIN
	SELECT hits, builds INTO h0, b0 FROM sift._test_col_gdead_stats();
	ok := allq();
	SELECT hits, builds INTO h1, b1 FROM sift._test_col_gdead_stats();
	INSERT INTO res VALUES (name, ok, h1 - h0, b1 - b0);
END $$;

SELECT step('clean');
-- the first query after rows were cleared makes the lists (builds), the second finds them (hits)
DELETE FROM t WHERE id % 37 = 0;
DELETE FROM p WHERE id % 37 = 0;
VACUUM t;
SELECT step('delete, vacuum');
SELECT step('again');
UPDATE t SET k = k + 1 WHERE id % 41 = 0;
UPDATE p SET k = k + 1 WHERE id % 41 = 0;
SELECT step('update, no vacuum');
VACUUM t;
SELECT step('update, vacuum');
SELECT step('again');
-- new rows take the freed addresses
INSERT INTO t (id, k, u) SELECT 40000 + i, (hashint4(i + 9) & 16777215) % 2000, (hashint4(i + 13) & 16777215) % 1000003 FROM generate_series(1, 3000) i;
INSERT INTO p SELECT * FROM t WHERE id >= 40000;
SELECT step('insert');
SELECT sift.col_promote('t_ix') >= 0 AS promoted;
SELECT step('promotion');
DELETE FROM t WHERE id % 43 = 0;
DELETE FROM p WHERE id % 43 = 0;
VACUUM t;
SELECT step('delete, vacuum');
SELECT step('again');
SELECT sift.col_merge('t_ix', true) >= 0 AS merged;
SELECT step('merge');
DELETE FROM t WHERE id % 47 = 0;
DELETE FROM p WHERE id % 47 = 0;
VACUUM t;
SELECT step('delete, vacuum');
SELECT step('again');
SELECT step, equal, hits > 0 AS found, builds > 0 AS made FROM res ORDER BY ctid;

-- a script of random writes, each followed by the queries (a statement of its own: VACUUM cannot share a transaction)
CREATE TABLE script (n serial, stmt text);
CREATE TABLE randres (n int, equal boolean);
INSERT INTO script (stmt)
SELECT stmt FROM (
	SELECT i, o, stmt FROM (
		SELECT i, CASE (random() * 5)::int
			WHEN 0 THEN ARRAY[format('DELETE FROM t WHERE id %% %s = %s', m, r), format('DELETE FROM p WHERE id %% %s = %s', m, r)]
			WHEN 1 THEN ARRAY[format('UPDATE t SET k = (hashint4(id + %s) & 16777215) %% 2000 WHERE id %% %s = %s', i, m, r), format('UPDATE p SET k = (hashint4(id + %s) & 16777215) %% 2000 WHERE id %% %s = %s', i, m, r)]
			WHEN 2 THEN ARRAY[format('INSERT INTO t (id, k, u) SELECT %s + g, (hashint4(g + %s) & 16777215) %% 2000, (hashint4(g + %s) & 16777215) %% 1000003 FROM generate_series(1, %s) g', 100000 + i * 5000, i, i + 3, 200 + m * 20),
						   format('INSERT INTO p SELECT * FROM t WHERE id > %s AND id <= %s', 100000 + i * 5000, 100000 + i * 5000 + 200 + m * 20)]
			WHEN 3 THEN ARRAY['SELECT sift.col_promote(''t_ix'') >= 0']
			ELSE ARRAY['VACUUM t'] END AS stmts
		FROM (SELECT i, 31 + (random() * 20)::int AS m, (random() * 10)::int AS r FROM generate_series(1, 36) i) s
	) a, unnest(a.stmts) WITH ORDINALITY u(stmt, o)
	UNION ALL SELECT i, 9, format('INSERT INTO randres SELECT %s, allq()', i) FROM generate_series(1, 36) i
) w ORDER BY i, o;
SELECT stmt FROM script ORDER BY n \gexec
SELECT count(*) AS runs, count(*) FILTER (WHERE equal) AS equal FROM randres;
SELECT sift.col_verify('t_ix');
DROP TABLE t, p, res, script, randres;
DROP FUNCTION same(text), allq(), step(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
