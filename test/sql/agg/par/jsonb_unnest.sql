-- LATERAL jsonb_to_recordset over the shredded leaves of a jsonb array of objects (format major 2) on the compute threads of the backend (core/par/pool.h): the leader copies the lengths and the columns of the
-- elements of a batch into a slot, the threads expand them into pairs and fold them. The rows the leaves cannot hold (a value of another JSON type than its column's), the elements the quals of the element
-- reject, rows deleted or updated after the build, and more groups than work_mem holds are the leader's, and the answer is the same. Every query is run with the node on 4 threads and with the aggregate scan off,
-- and the sorted rows are compared; the first word says they agree, the second that the plan is the node's, the third that the answer is not empty.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 3;
SET jit = off;
SET work_mem = '256MB';
SET sift.test_create_format1 = off;
SET sift.agg_force = on;
\ir test/common/sql/agg/base.sql
\set ECHO none
CREATE FUNCTION node_of(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;
CREATE FUNCTION check_par(name text, q text) RETURNS TABLE (label text, same bool, node bool, nonempty bool) LANGUAGE plpgsql AS $$
DECLARE a text := runq(q, true); b text := runq(q, false);
BEGIN
	RETURN QUERY SELECT name, a = b, node_of(q), b <> '[]' AND b NOT LIKE 'ERR%';
END $$;

-- items: SQL NULL, [], 1 to 5 objects (a NULL, a missing and a JSON null member, a long text); the rows 50001 to 55000 have members of another type than their column's (a number, a boolean and an object for text,
-- a string for an integer) and a few arrays of 3000 objects: the index cannot hold those, and reads the rows of their slabs from the heap. doc -> 'rows' is such an array too, or a document without the key
CREATE TABLE jr_t (id int, grp int, qty int, label text, items jsonb, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO jr_t
SELECT i, i % 50, CASE WHEN i % 7 = 0 OR i % 50 = 3 THEN NULL ELSE i % 9 END, CASE WHEN i % 11 = 0 THEN NULL ELSE 'label-' || i % 13 END, a.items,
	CASE WHEN i % 61 = 0 THEN NULL WHEN i % 59 = 0 THEN '{"name": "x"}' ELSE jsonb_build_object('name', 'n' || i % 5, 'rows', coalesce(a.items, '[]')) END
FROM generate_series(1, 60000) i, LATERAL (SELECT
	CASE WHEN i % 29 = 0 THEN NULL
		WHEN i % 23 = 0 THEN '[]'::jsonb
		WHEN i BETWEEN 50001 AND 55000 AND i % 997 = 0 THEN (SELECT jsonb_agg(jsonb_build_object('kind', 'bulk-' || k % 40, 'n', k % 100)) FROM generate_series(1, 3000) k)
		ELSE (SELECT jsonb_agg(CASE
				WHEN k = 1 AND i % 31 = 0 THEN '{}'::jsonb
				WHEN k = 1 AND i % 37 = 0 THEN jsonb_build_object('kind', NULL::text, 'n', k, 'score', 1.5)
				WHEN k = 2 AND i % 41 = 0 AND i BETWEEN 50001 AND 55000 THEN jsonb_build_object('kind', 7, 'n', '8', 'score', 2.5)
				WHEN k = 1 AND i % 43 = 0 AND i BETWEEN 50001 AND 55000 THEN jsonb_build_object('kind', jsonb_build_object('a', 1), 'n', 3)
				WHEN k = 1 AND i % 47 = 0 AND i BETWEEN 50001 AND 55000 THEN jsonb_build_object('kind', true, 'n', 2, 'extra', jsonb_build_object('x', jsonb_build_array(1, 2)))
				ELSE jsonb_build_object('kind', CASE WHEN i % 53 = 0 THEN 'a-much-longer-kind-value-' || k ELSE 'k' || ((i + k) % 17) END,
										'n', CASE WHEN (i + k) % 19 = 0 THEN NULL ELSE (i * k) % 12 END, 'score', ((i * k) % 100) / 8.0) END ORDER BY k)
			FROM generate_series(1, 1 + i % 5) k) END AS items) a;
CREATE INDEX jr_t_col ON jr_t USING sift_col (id, grp, qty, label, items, doc)
	WITH (paths = 'items[]{kind text, n int4, score float8}, doc.rows[]{kind text, n int4}', rollup_columns = 'off');
VACUUM (FREEZE, ANALYZE) jr_t;

SELECT * FROM check_par('text column', $q$ SELECT e.kind, count(*) FROM jr_t f CROSS JOIN LATERAL jsonb_to_recordset(f.items) AS e(kind text, n int) GROUP BY 1 $q$);
SELECT * FROM check_par('text and integer columns', $q$ SELECT e.kind, e.n, count(*), sum(f.qty) FROM jr_t f CROSS JOIN LATERAL jsonb_to_recordset(f.items) AS e(kind text, n int) GROUP BY 1, 2 $q$);
SELECT * FROM check_par('integer column, int row key', $q$ SELECT f.grp, e.n, count(*), sum(f.qty), count(f.label) FROM jr_t f CROSS JOIN LATERAL jsonb_to_recordset(f.items) AS e(kind text, n int) GROUP BY 1, 2 $q$);
SELECT * FROM check_par('text row key and text column', $q$ SELECT f.label, e.kind, count(*), sum(f.qty) FROM jr_t f CROSS JOIN LATERAL jsonb_to_recordset(f.items) AS e(kind text, n int) GROUP BY 1, 2 $q$);
SELECT * FROM check_par('count and sum of the columns', $q$ SELECT f.grp, count(*), count(e.kind), count(e.n), sum(e.n), avg(e.n) FROM jr_t f CROSS JOIN LATERAL jsonb_to_recordset(f.items) AS e(kind text, n int) GROUP BY 1 $q$);
SELECT * FROM check_par('sum of a column of the row with NULLs', $q$ SELECT e.n, count(*), sum(f.qty), avg(f.qty) FROM jr_t f CROSS JOIN LATERAL jsonb_to_recordset(f.items) AS e(kind text, n int) GROUP BY 1 $q$);
SELECT * FROM check_par('a column left unread', $q$ SELECT f.grp, count(*) FROM jr_t f CROSS JOIN LATERAL jsonb_to_recordset(f.items) AS e(kind text, n int, score float8) GROUP BY 1 $q$);
SELECT * FROM check_par('a float8 column', $q$ SELECT e.kind, count(*), sum(e.score) FROM jr_t f CROSS JOIN LATERAL jsonb_to_recordset(f.items) AS e(kind text, n int, score float8) GROUP BY 1 $q$);
SELECT * FROM check_par('left join, text column', $q$ SELECT e.kind, count(*), count(e.kind), sum(f.qty) FROM jr_t f LEFT JOIN LATERAL jsonb_to_recordset(f.items) AS e(kind text, n int) ON true GROUP BY 1 $q$);
SELECT * FROM check_par('left join, two columns', $q$ SELECT f.grp, e.kind, e.n, count(*) FROM jr_t f LEFT JOIN LATERAL jsonb_to_recordset(f.items) AS e(kind text, n int) ON true GROUP BY 1, 2, 3 $q$);
SELECT * FROM check_par('a qual of the integer column', $q$ SELECT e.kind, count(*), sum(f.qty) FROM jr_t f CROSS JOIN LATERAL jsonb_to_recordset(f.items) AS e(kind text, n int) WHERE e.n > 5 GROUP BY 1 $q$);
SELECT * FROM check_par('a qual of the text column', $q$ SELECT e.n, count(*) FROM jr_t f CROSS JOIN LATERAL jsonb_to_recordset(f.items) AS e(kind text, n int) WHERE e.kind LIKE 'k1%' GROUP BY 1 $q$);
SELECT * FROM check_par('quals of both columns, a NULL test and IN', $q$ SELECT e.kind, count(*) FROM jr_t f CROSS JOIN LATERAL jsonb_to_recordset(f.items) AS e(kind text, n int) WHERE e.n IS NOT NULL AND e.n <> 4 AND e.kind IN ('k3', 'k5', 'k7') GROUP BY 1 $q$);
SELECT * FROM check_par('a qual of the row', $q$ SELECT e.kind, count(*) FROM jr_t f CROSS JOIN LATERAL jsonb_to_recordset(f.items) AS e(kind text, n int) WHERE f.grp < 10 AND f.qty IS NOT NULL GROUP BY 1 $q$);
SELECT * FROM check_par('the array is a key of the document', $q$ SELECT e.kind, e.n, count(*) FROM jr_t f CROSS JOIN LATERAL jsonb_to_recordset(f.doc -> 'rows') AS e(kind text, n int) GROUP BY 1, 2 $q$);
SELECT * FROM check_par('the array is a key of the document, left join', $q$ SELECT e.kind, count(*), sum(f.qty) FROM jr_t f LEFT JOIN LATERAL jsonb_to_recordset(f.doc -> 'rows') AS e(kind text, n int) ON true GROUP BY 1 $q$);
SELECT * FROM check_par('a group for every row and element', $q$ SELECT f.id, e.kind, count(*) FROM jr_t f CROSS JOIN LATERAL jsonb_to_recordset(f.items) AS e(kind text, n int) WHERE f.id <= 20000 GROUP BY 1, 2 $q$);
SELECT * FROM check_par('a join with a table', $q$ SELECT g.name, count(*) FROM jr_t f CROSS JOIN LATERAL jsonb_to_recordset(f.items) AS e(kind text, n int) JOIN (VALUES ('k1', 'one'), ('k2', 'two')) g(kind, name) ON g.kind = e.kind GROUP BY 1 $q$);
SET work_mem = '64MB';
SELECT * FROM check_par('a group for every row and element, the groups outgrow the threads', $q$ SELECT f.id, e.kind, count(*) FROM jr_t f CROSS JOIN LATERAL jsonb_to_recordset(f.items) AS e(kind text, n int) GROUP BY 1, 2 $q$);
SET work_mem = '4MB';
SELECT * FROM check_par('work_mem holds no two threads', $q$ SELECT e.kind, e.n, count(*), sum(f.qty) FROM jr_t f CROSS JOIN LATERAL jsonb_to_recordset(f.items) AS e(kind text, n int) GROUP BY 1, 2 $q$);
SET work_mem = '256MB';

-- rows deleted, updated and inserted after the build
DELETE FROM jr_t WHERE id % 1000 = 7;
UPDATE jr_t SET items = '[{"kind": "u", "n": 1}, {"kind": "v", "n": 2}]' WHERE id % 1000 = 11;
UPDATE jr_t SET qty = 100 WHERE id % 1000 = 13;
INSERT INTO jr_t SELECT 60000 + i, i % 50, i % 9, 'label-' || i % 13, jsonb_build_array(jsonb_build_object('kind', 'new' || i % 3, 'n', i % 5)), NULL FROM generate_series(1, 2000) i;
SELECT * FROM check_par('after deletes, updates and inserts', $q$ SELECT e.kind, e.n, count(*), sum(f.qty) FROM jr_t f CROSS JOIN LATERAL jsonb_to_recordset(f.items) AS e(kind text, n int) GROUP BY 1, 2 $q$);
SELECT * FROM check_par('after deletes, updates and inserts, left join', $q$ SELECT f.grp, e.kind, count(*), count(e.n) FROM jr_t f LEFT JOIN LATERAL jsonb_to_recordset(f.items) AS e(kind text, n int) ON true GROUP BY 1, 2 $q$);
VACUUM jr_t;
SELECT * FROM check_par('after vacuum', $q$ SELECT e.kind, count(*), sum(f.qty) FROM jr_t f CROSS JOIN LATERAL jsonb_to_recordset(f.items) AS e(kind text, n int) GROUP BY 1 $q$);

-- members of every type the leaves hold
CREATE TABLE jr_ty (id int, items jsonb) WITH (autovacuum_enabled = false);
INSERT INTO jr_ty SELECT i, jsonb_build_array(
	jsonb_build_object('t', 'v' || i % 7, 'i2', i % 100, 'i4', i * 1000 % 7919, 'i8', i::bigint * 100000000000 % 12345678901, 'f', i / 3.0, 'b', i % 2 = 0),
	jsonb_build_object('t', NULL::text, 'i4', NULL::int, 'b', i % 3 = 0))
FROM generate_series(1, 30000) i;
CREATE INDEX jr_ty_col ON jr_ty USING sift_col (id, items) WITH (paths = 'items[]{t text, i2 int2, i4 int4, i8 int8, f float8, b bool}', rollup_columns = 'off');
VACUUM (FREEZE, ANALYZE) jr_ty;
SELECT * FROM check_par('text key', $q$ SELECT e.t, count(*) FROM jr_ty f CROSS JOIN LATERAL jsonb_to_recordset(f.items) AS e(t text, i2 int2, i4 int4, i8 int8, f float8, b bool) GROUP BY 1 $q$);
SELECT * FROM check_par('smallint key, sum of integer', $q$ SELECT e.i2, count(*), sum(e.i4) FROM jr_ty f CROSS JOIN LATERAL jsonb_to_recordset(f.items) AS e(t text, i2 int2, i4 int4, i8 int8, f float8, b bool) GROUP BY 1 $q$);
SELECT * FROM check_par('bigint key', $q$ SELECT e.i8, count(*) FROM jr_ty f CROSS JOIN LATERAL jsonb_to_recordset(f.items) AS e(t text, i2 int2, i4 int4, i8 int8, f float8, b bool) GROUP BY 1 $q$);
SELECT * FROM check_par('float8 key', $q$ SELECT e.f, count(*) FROM jr_ty f CROSS JOIN LATERAL jsonb_to_recordset(f.items) AS e(t text, i2 int2, i4 int4, i8 int8, f float8, b bool) GROUP BY 1 $q$);
SELECT * FROM check_par('quals of the bigint and the smallint', $q$ SELECT e.t, count(*) FROM jr_ty f CROSS JOIN LATERAL jsonb_to_recordset(f.items) AS e(t text, i2 int2, i4 int4, i8 int8, f float8, b bool) WHERE e.i8 > 1000000 AND e.i2 < 50 GROUP BY 1 $q$);

-- a document that is not an array of objects: PostgreSQL raises, and so does the statement on the node (a scalar, an object)
CREATE TABLE jr_bad (id int, a jsonb, b jsonb) WITH (autovacuum_enabled = false);
INSERT INTO jr_bad SELECT i, CASE WHEN i = 5000 THEN '5' ELSE '[{"kind": "a", "n": 1}]' END::jsonb, CASE WHEN i = 5000 THEN '{"kind": "a"}' ELSE '[{"kind": "b", "n": 2}]' END::jsonb FROM generate_series(1, 9000) i;
CREATE INDEX jr_bad_col ON jr_bad USING sift_col (id, a, b) WITH (paths = 'a[]{kind text, n int4}, b[]{kind text, n int4}', rollup_columns = 'off');
VACUUM (FREEZE, ANALYZE) jr_bad;
SELECT * FROM check_par('a scalar document', $q$ SELECT e.kind, count(*) FROM jr_bad f CROSS JOIN LATERAL jsonb_to_recordset(f.a) AS e(kind text, n int) GROUP BY 1 $q$);
SELECT * FROM check_par('an object document', $q$ SELECT e.kind, count(*) FROM jr_bad f CROSS JOIN LATERAL jsonb_to_recordset(f.b) AS e(kind text, n int) GROUP BY 1 $q$);
SELECT * FROM check_par('the same table with the bad row out of the way', $q$ SELECT e.kind, count(*) FROM jr_bad f CROSS JOIN LATERAL jsonb_to_recordset(f.b) AS e(kind text, n int) WHERE f.id < 5000 GROUP BY 1 $q$);
DROP FUNCTION check_par(text, text), node_of(text), outcome(text, bool), runs_in_scan(text), pushed(text), runq(text, bool);
DROP TABLE jr_t, jr_ty, jr_bad;
DROP EXTENSION pg_sift;
