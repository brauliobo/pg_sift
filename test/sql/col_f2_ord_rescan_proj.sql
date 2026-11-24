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

CREATE FUNCTION rp_a_all(phase text, probes int[] DEFAULT '{1,2,3,4,5,6,7,8,9,10,11,12}') RETURNS TABLE (phase_name text, probe text, rows bigint, same_as_heap bool, nl_inner_ordered bool) LANGUAGE sql AS $$
	SELECT phase, q.probe, h.n, a.n = h.n AND a.md5 = h.md5, a.inner_ordered
	FROM rp_a_q q, LATERAL rp_a_rows(q.q, true) a, LATERAL rp_a_rows(q.q, false) h WHERE q.n = ANY (probes) ORDER BY q.n
$$;

SELECT * FROM rp_a_all('cold');
SELECT * FROM rp_a_all('same parameters again');

CREATE FUNCTION rp_a_buffers(q text, kb int) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE line text; n bigint; old text := current_setting('sift.backend_cache_kb');
BEGIN
	PERFORM rp_a_plan(true);
	PERFORM set_config('sift.backend_cache_kb', kb::text, true);
	FOR i IN 1..2 LOOP
		n := NULL;
		FOR line IN EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
			IF n IS NULL AND line ~ 'Buffers: shared' THEN
				n := coalesce(substring(line from 'hit=(\d+)')::bigint, 0) + coalesce(substring(line from 'read=(\d+)')::bigint, 0);
			END IF;
		END LOOP;
	END LOOP;
	PERFORM set_config('sift.backend_cache_kb', old, true);
	PERFORM rp_a_plan_reset();
	RETURN n;
END $$;

SELECT q.probe, rp_a_buffers(q.q, 16384) < rp_a_buffers(q.q, 0) AS cache_cuts_buffer_reads FROM rp_a_q q WHERE q.n IN (2, 3, 7) ORDER BY q.n;

INSERT INTO rp_a_s SELECT 20000 + o.id, o.k, 7 FROM rp_a_o o WHERE o.id <= 20 AND o.k IS NOT NULL;
INSERT INTO rp_a_d SELECT 20000 + p.id, p.k FROM rp_a_dp p WHERE p.id <= 20;
INSERT INTO rp_a_c SELECT p.a, p.b, 20000 + p.id FROM rp_a_cp p WHERE p.id <= 20;
SELECT * FROM rp_a_all('after insert', '{1,2,3,4,6,9,10,11}');

UPDATE rp_a_s SET k = k + 1 WHERE id % 9 = 0;
UPDATE rp_a_d SET k = k - 2 WHERE id % 13 = 0;
UPDATE rp_a_c SET b = b + 1 WHERE id % 11 = 0;
SELECT * FROM rp_a_all('after update', '{1,2,3,4,6,9,10,11}');

DELETE FROM rp_a_s WHERE id % 4 = 1;
DELETE FROM rp_a_d WHERE id % 5 = 2;
DELETE FROM rp_a_c WHERE id % 6 = 3;
SELECT * FROM rp_a_all('after delete', '{1,2,3,4,6,9,10,11}');

VACUUM rp_a_s;
VACUUM rp_a_d;
VACUUM rp_a_c;
SELECT * FROM rp_a_all('after vacuum', '{1,2,3,4,6,9,10,11}');

BEGIN;
INSERT INTO rp_a_s SELECT 30000 + o.id, o.k, 5 FROM rp_a_o o WHERE o.id > 15 AND o.id <= 30 AND o.k IS NOT NULL;
DELETE FROM rp_a_s WHERE id % 3 = 0;
UPDATE rp_a_s SET k = k + 2 WHERE id % 10 = 4;
SELECT * FROM rp_a_all('own transaction', '{1,2,3,6,9}');
ABORT;
SELECT * FROM rp_a_all('after abort', '{1,3,6}');

BEGIN;
UPDATE rp_a_s SET k = k + 3 WHERE id % 7 = 0;
SAVEPOINT rp_a_sp;
DELETE FROM rp_a_s WHERE id % 3 = 1;
INSERT INTO rp_a_s SELECT 40000 + o.id, o.k, 1 FROM rp_a_o o WHERE o.id <= 25 AND o.k IS NOT NULL;
SELECT * FROM rp_a_all('inside savepoint', '{1,2,3,6,9}');
ROLLBACK TO rp_a_sp;
SELECT * FROM rp_a_all('after rollback to savepoint', '{1,3,6}');
RELEASE rp_a_sp;
COMMIT;
SELECT * FROM rp_a_all('after commit', '{1,2,3}');

INSERT INTO rp_a_s SELECT 50000 + o.id, o.k, 2 FROM rp_a_o o WHERE o.id <= 10 AND o.k IS NOT NULL;
SELECT * FROM rp_a_all('rows in the slab', '{1,2,3,6,9,11}');

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
SELECT * FROM rp_a_all('after the cursor', '{1,2,3}');
ROLLBACK;

REINDEX INDEX rp_a_s_k;
SELECT * FROM rp_a_all('after reindex', '{1,2,3}');

DROP FUNCTION rp_a_buffers(text, int), rp_a_all(text, int[]), rp_a_cursor(text, text), rp_a_rows(text, bool), rp_a_plan(bool), rp_a_plan_reset();
DROP TABLE rp_a_q, rp_a_s, rp_a_o, rp_a_d, rp_a_dp, rp_a_c, rp_a_cp, rp_a_u;

-- An ordered scan that stops early reads a few rows of each part instead of its first granule: MIN(k), MAX(k) and ORDER BY k LIMIT n over a sift_col index of format major 2 whose
-- parts overlap (two parts and the L0 slab) read the rows of a window of each granule, small first and doubling, and the addresses of the rows read only. Every query runs twice, as the planner is made
-- to run it (an ordered index scan, no Sort) and on the heap (a sequential scan and a Sort), and the rows must be the same, after inserts, deletes, updates, an abort, a vacuum and a transaction's own changes.
-- The same holds for a sort projection (SiftProjSortScan): a part is read a window of its granule at a time, with and without a range of the sort column, with the projection's rows, the slab and the rows changed since.
SET timezone = 'UTC';
SET sift.async_maintenance = off;

-- k is unique where it is not NULL, so that a LIMIT cuts the same rows in both plans; two parts (a vacuum between the first two inserts) and the slab (the third insert)
CREATE TABLE rp_b_a (id int, k int4, v int4);
CREATE INDEX rp_b_a_k ON rp_b_a USING sift_col (k sift.col_int4_ops) WITH (key_order = 'k');
INSERT INTO rp_b_a SELECT i, CASE WHEN i % 13 = 0 THEN NULL ELSE ((i::int8 * 7919) % 100003)::int END, i % 50 FROM generate_series(1, 8300) i;
VACUUM rp_b_a;
INSERT INTO rp_b_a SELECT i, CASE WHEN i % 13 = 0 THEN NULL ELSE ((i::int8 * 7919) % 100003)::int END, i % 50 FROM generate_series(8301, 9900) i;
VACUUM rp_b_a;
INSERT INTO rp_b_a SELECT i, CASE WHEN i % 13 = 0 THEN NULL ELSE ((i::int8 * 7919) % 100003)::int END, i % 50 FROM generate_series(9901, 10600) i;
ANALYZE rp_b_a;
CREATE TABLE rp_b_d (id int, k int8, v int4);
CREATE INDEX rp_b_d_k ON rp_b_d USING sift_col (k sift.col_int8_ops DESC NULLS LAST) WITH (key_order = 'k DESC NULLS LAST');
INSERT INTO rp_b_d SELECT i, CASE WHEN i % 11 = 0 THEN NULL ELSE (i::int8 * 104729) % 50021 END, i % 7 FROM generate_series(1, 1500) i;
VACUUM (ANALYZE) rp_b_d;

CREATE FUNCTION rp_b_run(q text, ord bool, OUT n bigint, OUT md5 text, OUT scans int, OUT sorts int) LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'off', true);
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	PERFORM set_config('enable_seqscan', (NOT ord)::text, true);
	PERFORM set_config('enable_indexscan', ord::text, true);
	PERFORM set_config('enable_indexonlyscan', ord::text, true);
	PERFORM set_config('enable_sort', (NOT ord)::text, true);
	PERFORM set_config('enable_bitmapscan', 'false', true);
	scans := 0;
	sorts := 0;
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF line ~ 'Index (Only )?Scan (Backward )?using rp_b_._k' THEN scans := scans + 1; END IF;
		IF line ~ '^ *(->  )?(Incremental )?Sort$' THEN sorts := sorts + 1; END IF;
	END LOOP;
	EXECUTE 'SELECT count(*), md5(string_agg(w::text, ''|'')) FROM (' || q || ') w' INTO n, md5;
END $$;

CREATE FUNCTION rp_b_check(q text, OUT rows bigint, OUT agree bool, OUT index_scans int, OUT sorts int) LANGUAGE plpgsql AS $$
DECLARE a record; h record;
BEGIN
	SELECT * INTO a FROM rp_b_run(q, true);
	SELECT * INTO h FROM rp_b_run(q, false);
	rows := h.n; agree := a.n = h.n AND a.md5 IS NOT DISTINCT FROM h.md5; index_scans := a.scans; sorts := a.sorts;
	IF NOT agree THEN
		RAISE EXCEPTION 'the ordered scan gives other rows than the heap: % (% against % rows)', q, a.n, h.n;
	END IF;
END $$;

CREATE TABLE rp_b_q (n int, q text);
INSERT INTO rp_b_q VALUES
 (1, 'SELECT k FROM rp_b_a ORDER BY k LIMIT 10'),
 (2, 'SELECT k FROM rp_b_a ORDER BY k LIMIT 1000'),
 (3, 'SELECT k FROM rp_b_a ORDER BY k LIMIT 3000 OFFSET 500'),
 (4, 'SELECT k FROM rp_b_a ORDER BY k DESC LIMIT 10'),
 (5, 'SELECT k FROM rp_b_a ORDER BY k DESC LIMIT 1000'),
 (6, 'SELECT min(k) FROM rp_b_a'),
 (7, 'SELECT max(k) FROM rp_b_a'),
 (8, 'SELECT k FROM rp_b_a WHERE k > 50000 ORDER BY k LIMIT 10'),
 (9, 'SELECT k FROM rp_b_a WHERE k BETWEEN 100 AND 5000 ORDER BY k LIMIT 700'),
 (10, 'SELECT k FROM rp_b_a WHERE k < 300 ORDER BY k DESC LIMIT 10'),
 (11, 'SELECT k FROM rp_b_a WHERE k > 99990 ORDER BY k'),
 (12, 'SELECT k FROM rp_b_a WHERE k = 7919'),
 (13, 'SELECT id, k FROM rp_b_a WHERE k IS NOT NULL ORDER BY k LIMIT 100'),
 (14, 'SELECT id, k FROM rp_b_a WHERE k IS NOT NULL ORDER BY k DESC LIMIT 100'),
 (15, 'SELECT id, k, v FROM rp_b_a WHERE k > 20000 AND v = 7 ORDER BY k LIMIT 25'),
 (16, 'SELECT k FROM rp_b_a ORDER BY k LIMIT 10500'),
 (17, 'SELECT k FROM rp_b_d ORDER BY k DESC NULLS LAST LIMIT 10'),
 (18, 'SELECT k FROM rp_b_d ORDER BY k DESC NULLS LAST LIMIT 700'),
 (19, 'SELECT k FROM rp_b_d ORDER BY k NULLS FIRST LIMIT 10'),
 (20, 'SELECT min(k) FROM rp_b_d'),
 (21, 'SELECT max(k) FROM rp_b_d'),
 (22, 'SELECT k FROM rp_b_d WHERE k < 40000 ORDER BY k DESC NULLS LAST LIMIT 40');
-- one index scan and no Sort in every plan
SELECT n, q, (rp_b_check(q)).* FROM rp_b_q ORDER BY n;

-- rows deleted, updated and inserted after the parts were written (the deletion vectors, the pending deletes and the slab)
DELETE FROM rp_b_a WHERE id % 10 = 3;
UPDATE rp_b_a SET k = k + 100003 WHERE id % 10 = 4;
UPDATE rp_b_a SET k = NULL WHERE id % 10 = 5;
INSERT INTO rp_b_a SELECT 60000 + i, ((i::int8 * 31337) % 100003)::int + 200000, i % 50 FROM generate_series(1, 300) i;
DELETE FROM rp_b_d WHERE id % 9 = 2;
INSERT INTO rp_b_d SELECT 3000 + i, (i::int8 * 31) % 50021, i % 7 FROM generate_series(1, 100) i;
SELECT n, q, (rp_b_check(q)).* FROM rp_b_q WHERE n IN (1, 4, 6, 7, 13, 15, 16, 17, 20) ORDER BY n;

-- a transaction's own changes, then rolled back
BEGIN;
DELETE FROM rp_b_a WHERE id % 7 = 1;
UPDATE rp_b_a SET k = 400000 - id WHERE id % 7 = 2;
INSERT INTO rp_b_a SELECT 90000 + i, -1000 - i, 1 FROM generate_series(1, 60) i;
SELECT n, q, (rp_b_check(q)).* FROM rp_b_q WHERE n IN (1, 6, 11) ORDER BY n;
ROLLBACK;
SELECT n, q, (rp_b_check(q)).* FROM rp_b_q WHERE n IN (1, 6, 11) ORDER BY n;

-- after a vacuum: the parts are rewritten and the pages are all visible
VACUUM rp_b_a;
VACUUM rp_b_d;
SELECT n, q, (rp_b_check(q)).* FROM rp_b_q WHERE n IN (1, 6, 14, 17, 20) ORDER BY n;


-- a sort projection of an index: two parts it was built from, and the L0 slab it has no row of; a range of k, MIN and a LIMIT with an OFFSET are read from it
CREATE TABLE rp_b_p (id int, k int4, v int4);
CREATE INDEX rp_b_p_idx ON rp_b_p USING sift_col (id, k, v) WITH (key_order = 'id');
INSERT INTO rp_b_p SELECT i, CASE WHEN i % 13 = 0 THEN NULL ELSE ((i::int8 * 7919) % 100003)::int END, i % 50 FROM generate_series(1, 2000) i;
SELECT sift.create_projection('rp_b_p_idx', 'by_k', 'ORDER BY k, id');
VACUUM rp_b_p;
INSERT INTO rp_b_p SELECT i, CASE WHEN i % 13 = 0 THEN NULL ELSE ((i::int8 * 7919) % 100003)::int END, i % 50 FROM generate_series(2001, 4000) i;
VACUUM rp_b_p;
INSERT INTO rp_b_p SELECT i, CASE WHEN i % 13 = 0 THEN NULL ELSE ((i::int8 * 7919) % 100003)::int END, i % 50 FROM generate_series(4001, 5000) i;
ANALYZE rp_b_p;
SELECT name, kind, state, rows FROM sift.projection_list() WHERE relation = 'rp_b_p_idx'::regclass;

-- the plan reads the projection (the first line of each EXPLAIN that names it) and the rows are those of the heap (the order by an expression the projection does not have)
CREATE FUNCTION rp_b_prun(q text, h text, OUT rows bigint, OUT agree bool, OUT node text, OUT source text, OUT index_only bool, OUT fetched bool) LANGUAGE plpgsql AS $$
DECLARE line text; a record; b record;
BEGIN
	index_only := false; fetched := false;
	PERFORM set_config('sift.enable_agg', 'off', true);
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	PERFORM set_config('enable_seqscan', 'false', true);
	PERFORM set_config('enable_bitmapscan', 'false', true);
	FOR line IN EXECUTE 'EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF line ~ 'SiftProjSortScan' THEN node := 'SiftProjSortScan'; END IF;
		IF line ~ '^ *Source: ' THEN source := btrim(substr(line, position('Source: ' IN line) + 8)); END IF;
		IF line ~ '^ *Index Only: true' THEN index_only := true; END IF;
		IF line ~ '^ *Heap Fetches: ' THEN fetched := substr(line, position('Heap Fetches: ' IN line) + 14)::bigint > 0; END IF;
	END LOOP;
	EXECUTE 'SELECT count(*), md5(string_agg(w::text, ''|'')) FROM (' || q || ') w' INTO a;
	PERFORM set_config('enable_seqscan', 'true', true);
	PERFORM set_config('enable_indexscan', 'false', true);
	PERFORM set_config('enable_indexonlyscan', 'false', true);
	EXECUTE 'SELECT count(*), md5(string_agg(w::text, ''|'')) FROM (' || h || ') w' INTO b;
	rows := b.count; agree := a.count = b.count AND a.md5 IS NOT DISTINCT FROM b.md5;
	IF NOT agree THEN
		RAISE EXCEPTION 'the projection gives other rows than the heap: % (% against % rows)', q, a.count, b.count;
	END IF;
END $$;

CREATE TABLE rp_b_pq (n int, q text, h text);
INSERT INTO rp_b_pq VALUES
 (1, 'SELECT id, k FROM rp_b_p ORDER BY k, id LIMIT 10', 'SELECT id, k FROM rp_b_p ORDER BY k + 0, id LIMIT 10'),
 (2, 'SELECT id, k FROM rp_b_p ORDER BY k, id LIMIT 1000', 'SELECT id, k FROM rp_b_p ORDER BY k + 0, id LIMIT 1000'),
 (3, 'SELECT id, k FROM rp_b_p ORDER BY k, id LIMIT 20 OFFSET 700', 'SELECT id, k FROM rp_b_p ORDER BY k + 0, id LIMIT 20 OFFSET 700'),
 (4, 'SELECT id, k, v FROM rp_b_p ORDER BY k, id LIMIT 1500', 'SELECT id, k, v FROM rp_b_p ORDER BY k + 0, id LIMIT 1500'),
 (5, 'SELECT min(k) FROM rp_b_p', 'SELECT min(k + 0) FROM rp_b_p'),
 (6, 'SELECT id, k FROM rp_b_p WHERE k > 50000 ORDER BY k, id LIMIT 10', 'SELECT id, k FROM rp_b_p WHERE k > 50000 ORDER BY k + 0, id LIMIT 10'),
 (7, 'SELECT id, k FROM rp_b_p WHERE k BETWEEN 100 AND 5000 ORDER BY k, id LIMIT 700', 'SELECT id, k FROM rp_b_p WHERE k BETWEEN 100 AND 5000 ORDER BY k + 0, id LIMIT 700'),
 (8, 'SELECT id, k FROM rp_b_p WHERE k = 7919 ORDER BY k, id LIMIT 5', 'SELECT id, k FROM rp_b_p WHERE k = 7919 ORDER BY k + 0, id LIMIT 5'),
 (9, 'SELECT id, k FROM rp_b_p WHERE k IN (5, 7919, 31337, 99999) ORDER BY k, id LIMIT 20', 'SELECT id, k FROM rp_b_p WHERE k IN (5, 7919, 31337, 99999) ORDER BY k + 0, id LIMIT 20'),
 (10, 'SELECT id, k FROM rp_b_p WHERE k > 99990 ORDER BY k, id LIMIT 4000', 'SELECT id, k FROM rp_b_p WHERE k > 99990 ORDER BY k + 0, id LIMIT 4000'),
 (11, 'SELECT id, k, v FROM rp_b_p WHERE k < 20000 AND v = 7 ORDER BY k, id LIMIT 25', 'SELECT id, k, v FROM rp_b_p WHERE k < 20000 AND v = 7 ORDER BY k + 0, id LIMIT 25'),
 (12, 'SELECT id, k FROM rp_b_p WHERE k IS NULL ORDER BY k, id LIMIT 30', 'SELECT id, k FROM rp_b_p WHERE k IS NULL ORDER BY k + 0, id LIMIT 30');
SELECT n, (rp_b_prun(q, h)).* FROM rp_b_pq ORDER BY n;

-- the statement reads integer columns the projection keeps: the rows of pages the map of the relation says are all visible come from the cells (Heap Fetches counts the others, fetched says there were some); rows inserted
-- since the vacuum are in pages it does not know, and the vacuum sets them
INSERT INTO rp_b_p SELECT 30000 + i, -i, 1 FROM generate_series(1, 40) i;
SELECT n, (rp_b_prun(q, h)).* FROM rp_b_pq WHERE n IN (1, 5, 6) ORDER BY n;
VACUUM rp_b_p;
SELECT n, (rp_b_prun(q, h)).* FROM rp_b_pq WHERE n IN (1, 5, 6) ORDER BY n;
-- a qual only the row can decide is read from the relation, and so is every row of the statement
SELECT (rp_b_prun('SELECT id, k FROM rp_b_p WHERE v + 1 = 8 ORDER BY k, id LIMIT 10', 'SELECT id, k FROM rp_b_p WHERE v + 1 = 8 ORDER BY k + 0, id LIMIT 10')).*;

-- rows deleted, updated and inserted since the parts were built (deletion vectors, pending deletes, the slab and the heap pages that changed)
DELETE FROM rp_b_p WHERE id % 10 = 3;
UPDATE rp_b_p SET k = k + 100003 WHERE id % 10 = 4;
UPDATE rp_b_p SET k = NULL WHERE id % 10 = 5;
INSERT INTO rp_b_p SELECT 40000 + i, ((i::int8 * 31337) % 100003)::int, i % 50 FROM generate_series(1, 200) i;
SELECT n, (rp_b_prun(q, h)).* FROM rp_b_pq WHERE n IN (1, 5, 12) ORDER BY n;

-- a transaction's own changes, then rolled back
BEGIN;
DELETE FROM rp_b_p WHERE id % 7 = 1;
UPDATE rp_b_p SET k = 400000 - id WHERE id % 7 = 2;
INSERT INTO rp_b_p SELECT 90000 + i, -1000 - i, 1 FROM generate_series(1, 60) i;
SELECT n, (rp_b_prun(q, h)).* FROM rp_b_pq WHERE n IN (1, 5) ORDER BY n;
ROLLBACK;
SELECT n, (rp_b_prun(q, h)).* FROM rp_b_pq WHERE n IN (1, 5) ORDER BY n;

-- after a vacuum: the changed parts are rebuilt and the projection serves them again
VACUUM rp_b_p;
SELECT n, (rp_b_prun(q, h)).* FROM rp_b_pq WHERE n IN (1, 3, 5, 7, 9, 12) ORDER BY n;

DROP FUNCTION rp_b_run(text, bool), rp_b_check(text), rp_b_prun(text, text);
DROP TABLE rp_b_q, rp_b_a, rp_b_d, rp_b_pq, rp_b_p;

DROP EXTENSION pg_sift;
