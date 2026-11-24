CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;

CREATE TABLE rp_a_s (id int, k int4, v int4);
INSERT INTO rp_a_s SELECT i, (i * 7919) % 20011, i % 10 FROM generate_series(1, 9000) i;
CREATE INDEX rp_a_s_k ON rp_a_s USING sift_col (k sift.col_int4_ops) WITH (key_order = 'k');
INSERT INTO rp_a_s SELECT i, (i * 7919) % 20011, i % 10 FROM generate_series(9001, 9800) i;
VACUUM rp_a_s;
INSERT INTO rp_a_s SELECT i, (i * 7919) % 20011, i % 10 FROM generate_series(9801, 10500) i;
VACUUM (ANALYZE) rp_a_s;
CREATE TABLE rp_a_o AS SELECT i AS id, ((i * 104729) % 20011)::int4 AS k FROM generate_series(1, 30) i;
INSERT INTO rp_a_o SELECT 30 + id, k FROM rp_a_o WHERE id <= 5;
INSERT INTO rp_a_o VALUES (36, NULL), (37, -5), (38, 30000);
ANALYZE rp_a_o;

CREATE TABLE rp_a_d (id int, k int4);
INSERT INTO rp_a_d SELECT i, CASE WHEN i % 11 = 0 THEN NULL ELSE (i * 31) % 997 - 400 END FROM generate_series(1, 3000) i;
CREATE INDEX rp_a_d_k ON rp_a_d USING sift_col (k sift.col_int4_ops DESC NULLS FIRST) WITH (key_order = 'k DESC NULLS FIRST');
INSERT INTO rp_a_d SELECT 3000 + i, CASE WHEN i % 7 = 0 THEN NULL ELSE (i * 17) % 997 - 400 END FROM generate_series(1, 1000) i;
VACUUM (ANALYZE) rp_a_d;
CREATE TABLE rp_a_dp AS SELECT i AS id, (i * 53 % 1100 - 450)::int4 AS k FROM generate_series(1, 25) i;
ANALYZE rp_a_dp;

CREATE TABLE rp_a_c (a int4, b int4, id int);
INSERT INTO rp_a_c SELECT i % 40, (i * 17) % 100, i FROM generate_series(1, 3000) i;
CREATE INDEX rp_a_c_ab ON rp_a_c USING sift_col (a sift.col_int4_ops, b sift.col_int4_ops) WITH (key_order = 'a, b');
VACUUM (ANALYZE) rp_a_c;
CREATE TABLE rp_a_cp AS SELECT i AS id, (i % 45)::int4 AS a, ((i * 13) % 100)::int4 AS b FROM generate_series(1, 25) i;
ANALYZE rp_a_cp;

CREATE TABLE rp_a_u (k int4, pad text);
INSERT INTO rp_a_u SELECT i * 3, 'x' FROM generate_series(1, 3000) i;
CREATE INDEX rp_a_u_k ON rp_a_u USING sift_col (k sift.col_int4_ops) WITH (key_order = 'k');
VACUUM (ANALYZE) rp_a_u;

CREATE FUNCTION rp_a_plan(ord bool) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.enable_agg', 'off', true);
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	PERFORM set_config('enable_seqscan', (NOT ord)::text, true);
	PERFORM set_config('enable_indexscan', ord::text, true);
	PERFORM set_config('enable_indexonlyscan', ord::text, true);
	PERFORM set_config('enable_bitmapscan', (NOT ord)::text, true);
	PERFORM set_config('enable_hashjoin', (NOT ord)::text, true);
	PERFORM set_config('enable_mergejoin', 'false', true);
	PERFORM set_config('enable_material', 'false', true);
	PERFORM set_config('enable_nestloop', ord::text, true);
END $$;

CREATE FUNCTION rp_a_plan_reset() RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	PERFORM set_config('enable_seqscan', 'true', true);
	PERFORM set_config('enable_indexscan', 'true', true);
	PERFORM set_config('enable_indexonlyscan', 'true', true);
	PERFORM set_config('enable_bitmapscan', 'true', true);
	PERFORM set_config('enable_hashjoin', 'true', true);
	PERFORM set_config('enable_mergejoin', 'true', true);
	PERFORM set_config('enable_material', 'true', true);
	PERFORM set_config('enable_nestloop', 'true', true);
END $$;

CREATE FUNCTION rp_a_rows(q text, ord bool, OUT n bigint, OUT md5 text, OUT inner_ordered bool) LANGUAGE plpgsql AS $$
DECLARE line text; plan text := '';
BEGIN
	PERFORM rp_a_plan(ord);
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		plan := plan || line || E'\n';
	END LOOP;
	inner_ordered := plan ~ 'Nested Loop' AND plan ~ 'Index (Only )?Scan (Backward )?using rp_a_';
	EXECUTE 'SELECT count(*), coalesce(md5(string_agg(w::text, ''|'' ORDER BY w::text)), '''') FROM (' || q || ') w' INTO n, md5;
	PERFORM rp_a_plan_reset();
END $$;

CREATE TABLE rp_a_q (n int, probe text, q text);
INSERT INTO rp_a_q VALUES
 (1, 'equal, index only', 'SELECT o.id, s.k FROM rp_a_o o JOIN rp_a_s s ON s.k = o.k'),
 (2, 'equal, heap rows', 'SELECT o.id, s.id, s.v FROM rp_a_o o JOIN rp_a_s s ON s.k = o.k'),
 (3, 'range', 'SELECT o.id, s.id FROM rp_a_o o JOIN rp_a_s s ON s.k >= o.k AND s.k < o.k + 6'),
 (4, 'range and filter', 'SELECT o.id, s.id FROM rp_a_o o JOIN rp_a_s s ON s.k BETWEEN o.k AND o.k + 40 AND s.v = 3'),
 (5, 'range below', 'SELECT o.id, s.id FROM rp_a_o o JOIN rp_a_s s ON s.k <= o.k AND s.k > o.k - 5'),
 (6, 'descending, equal', 'SELECT p.id, d.id FROM rp_a_dp p JOIN rp_a_d d ON d.k = p.k'),
 (7, 'descending, range', 'SELECT p.id, d.id FROM rp_a_dp p JOIN rp_a_d d ON d.k >= p.k AND d.k < p.k + 3'),
 (8, 'descending, range below', 'SELECT p.id, d.id FROM rp_a_dp p JOIN rp_a_d d ON d.k <= p.k AND d.k > p.k - 4'),
 (9, 'two columns, equal', 'SELECT c.id, t.id FROM rp_a_cp c JOIN rp_a_c t ON t.a = c.a AND t.b = c.b'),
 (10, 'two columns, range', 'SELECT c.id, t.id FROM rp_a_cp c JOIN rp_a_c t ON t.a = c.a AND t.b BETWEEN c.b AND c.b + 3'),
 (11, 'next keys', 'SELECT o.id, t.k FROM rp_a_o o, LATERAL (SELECT u.k FROM rp_a_u u WHERE u.k >= o.k ORDER BY u.k LIMIT 3) t'),
 (12, 'previous keys', 'SELECT o.id, t.k FROM rp_a_o o, LATERAL (SELECT u.k FROM rp_a_u u WHERE u.k <= o.k ORDER BY u.k DESC LIMIT 2) t');

CREATE FUNCTION rp_a_all(phase text, upto int DEFAULT 12) RETURNS TABLE (phase_name text, probe text, rows bigint, same_as_heap bool, nl_inner_ordered bool) LANGUAGE sql AS $$
	SELECT phase, q.probe, h.n, a.n = h.n AND a.md5 = h.md5, a.inner_ordered
	FROM rp_a_q q, LATERAL rp_a_rows(q.q, true) a, LATERAL rp_a_rows(q.q, false) h WHERE q.n <= upto ORDER BY q.n
$$;

SELECT * FROM rp_a_all('cold');
SELECT * FROM rp_a_all('same parameters again');

INSERT INTO rp_a_s SELECT 20000 + o.id, o.k, 7 FROM rp_a_o o WHERE o.id <= 20 AND o.k IS NOT NULL;
INSERT INTO rp_a_d SELECT 20000 + p.id, p.k FROM rp_a_dp p WHERE p.id <= 20;
INSERT INTO rp_a_c SELECT p.a, p.b, 20000 + p.id FROM rp_a_cp p WHERE p.id <= 20;
SELECT * FROM rp_a_all('after insert');

UPDATE rp_a_s SET k = k + 1 WHERE id % 9 = 0;
UPDATE rp_a_d SET k = k - 2 WHERE id % 13 = 0;
UPDATE rp_a_c SET b = b + 1 WHERE id % 11 = 0;
SELECT * FROM rp_a_all('after update');

DELETE FROM rp_a_s WHERE id % 4 = 1;
DELETE FROM rp_a_d WHERE id % 5 = 2;
DELETE FROM rp_a_c WHERE id % 6 = 3;
SELECT * FROM rp_a_all('after delete');

VACUUM rp_a_s;
VACUUM rp_a_d;
VACUUM rp_a_c;
SELECT * FROM rp_a_all('after vacuum');

BEGIN;
INSERT INTO rp_a_s SELECT 30000 + o.id, o.k, 5 FROM rp_a_o o WHERE o.id > 15 AND o.id <= 30 AND o.k IS NOT NULL;
DELETE FROM rp_a_s WHERE id % 3 = 0;
UPDATE rp_a_s SET k = k + 2 WHERE id % 10 = 4;
SELECT * FROM rp_a_all('own transaction', 9);
ABORT;
SELECT * FROM rp_a_all('after abort', 6);

BEGIN;
UPDATE rp_a_s SET k = k + 3 WHERE id % 7 = 0;
SAVEPOINT rp_a_sp;
DELETE FROM rp_a_s WHERE id % 3 = 1;
INSERT INTO rp_a_s SELECT 40000 + o.id, o.k, 1 FROM rp_a_o o WHERE o.id <= 25 AND o.k IS NOT NULL;
SELECT * FROM rp_a_all('inside savepoint', 9);
ROLLBACK TO rp_a_sp;
SELECT * FROM rp_a_all('after rollback to savepoint', 6);
RELEASE rp_a_sp;
COMMIT;
SELECT * FROM rp_a_all('after commit', 3);

INSERT INTO rp_a_s SELECT 50000 + o.id, o.k, 2 FROM rp_a_o o WHERE o.id <= 10 AND o.k IS NOT NULL;
SELECT * FROM rp_a_all('rows in the slab');

CREATE FUNCTION rp_a_cursor(q text, dml text, OUT cursor_rows bigint, OUT same_as_heap bool, OUT fresh_rows bigint) LANGUAGE plpgsql AS $$
DECLARE ca refcursor; ch refcursor; x text; ra text[] := '{}'; rh text[] := '{}';
BEGIN
	PERFORM rp_a_plan(true);
	OPEN ca FOR EXECUTE q;
	PERFORM rp_a_plan(false);
	OPEN ch FOR EXECUTE q;
	PERFORM rp_a_plan_reset();
	EXECUTE dml;
	LOOP
		FETCH ca INTO x;
		EXIT WHEN NOT FOUND;
		ra := ra || x;
	END LOOP;
	LOOP
		FETCH ch INTO x;
		EXIT WHEN NOT FOUND;
		rh := rh || x;
	END LOOP;
	cursor_rows := cardinality(rh);
	same_as_heap := (SELECT array_agg(e ORDER BY e) FROM unnest(ra) e) IS NOT DISTINCT FROM (SELECT array_agg(e ORDER BY e) FROM unnest(rh) e);
	EXECUTE 'SELECT count(*) FROM (' || q || ') w' INTO fresh_rows;
END $$;

BEGIN;
SELECT * FROM rp_a_cursor('SELECT o.id::text || ''/'' || s.id::text FROM rp_a_o o JOIN rp_a_s s ON s.k = o.k', 'DELETE FROM rp_a_s WHERE id % 2 = 0');
SELECT * FROM rp_a_all('after the cursor', 3);
ROLLBACK;

REINDEX INDEX rp_a_s_k;
SELECT * FROM rp_a_all('after reindex', 3);

DROP FUNCTION rp_a_all(text, int), rp_a_cursor(text, text), rp_a_rows(text, bool), rp_a_plan(bool), rp_a_plan_reset();
DROP TABLE rp_a_q, rp_a_s, rp_a_o, rp_a_d, rp_a_dp, rp_a_c, rp_a_cp, rp_a_u;
DROP EXTENSION pg_sift;
