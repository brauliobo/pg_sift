-- SiftAggV6, live over an index of format major 2 (src/agg/): the aggregate of one table whose sift_col index of format major 2 holds the columns it reads is answered by the
-- scan of the index (its pruning, filters on the stored values, deletion vectors and pending deletes) with the rows the heap does not show to the snapshot left out, and folded in batches
-- into the groups. Every query is run with the node and without it (sift.enable_agg) and the two answers are compared; the first word says they agree, the second that the plan is the node's.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET sift.test_create_format1 = off;
-- the tables are tiny: the open of the scan would cost more than the heap plans these tests are not about
SET cpu_operator_cost = 0.00001;

CREATE TABLE lv (id int PRIMARY KEY, g int, t text, v bigint, f float8, d date, ts timestamptz, b bool, n int) WITH (autovacuum_enabled = false);
INSERT INTO lv SELECT i, i % 7, CASE WHEN i % 11 = 0 THEN NULL ELSE 'key' || (i % 13) || repeat('x', i % 20) END, CASE WHEN i % 17 = 0 THEN NULL ELSE i * 3 END,
	i * 0.5, DATE '2020-01-01' + (i % 400), TIMESTAMPTZ '2021-01-01' + i * interval '1 hour', i % 3 = 0, i % 100 FROM generate_series(1, 30000) i;
CREATE INDEX lv_col ON lv USING sift_col (g, t, v, f, d, ts, b, n, id);
UPDATE lv SET v = v + 1 WHERE id % 50 = 0;
DELETE FROM lv WHERE id % 97 = 0;
VACUUM lv;
ANALYZE lv;
INSERT INTO lv SELECT i, i % 7, 'new' || i, i, i, DATE '2022-01-01', TIMESTAMPTZ '2022-01-01', true, 5 FROM generate_series(30001, 30500) i;
SELECT sift._test_index_format('lv_col') AS format_major;

CREATE FUNCTION same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text; plan text := ''; l text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || l; END LOOP;
	EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO b;
	RETURN (CASE WHEN a IS DISTINCT FROM b THEN 'DIFFERENT' ELSE 'same' END) || ' ' || (CASE WHEN plan LIKE '%SiftAggV6%' THEN 'node' ELSE 'normal' END) || ' ' || q;
END $$;

-- 1. Ungrouped and grouped aggregates of every column type, NULL keys and NULL values, the rows an UPDATE and a DELETE ended and those an INSERT after the VACUUM added
SELECT same(q) FROM unnest(ARRAY[
	'SELECT count(*) FROM lv',
	'SELECT count(*), count(v), sum(v), min(v), max(v), avg(v) FROM lv',
	'SELECT g, count(*), sum(v), avg(v), min(n), max(n) FROM lv GROUP BY g',
	'SELECT t, count(*), sum(n) FROM lv GROUP BY t',
	'SELECT g, t, count(*) FROM lv GROUP BY g, t',
	'SELECT g, sum(f), avg(f), min(f), max(f) FROM lv GROUP BY g',
	'SELECT d, count(*) FROM lv GROUP BY d',
	'SELECT ts, count(*) FROM lv WHERE ts < ''2021-02-01'' GROUP BY ts',
	'SELECT b, count(*), sum(n) FROM lv GROUP BY b',
	'SELECT g, min(d), max(d), min(ts), max(ts) FROM lv GROUP BY g',
	'SELECT g, sum(v)::numeric / count(*) FROM lv GROUP BY g HAVING count(*) > 4000'
]) q;

-- 2. The quals the scan decides on the stored values: ranges, lists, NULL tests, an equality on a text, a window of dates
SELECT same(q) FROM unnest(ARRAY[
	'SELECT g, count(*) FROM lv WHERE n > 50 GROUP BY g',
	'SELECT g, count(*) FROM lv WHERE n IN (1, 2, 3, 50) GROUP BY g',
	'SELECT g, count(*) FROM lv WHERE v IS NULL GROUP BY g',
	'SELECT g, count(*) FROM lv WHERE t IS NOT NULL AND t = ''key3'' GROUP BY g',
	'SELECT g, count(*) FROM lv WHERE id > 29000 GROUP BY g',
	'SELECT g, count(*) FROM lv WHERE d >= ''2020-06-01'' AND d < ''2020-07-01'' GROUP BY g',
	'SELECT g, count(*) FROM lv WHERE n > 1000 GROUP BY g',
	'SELECT count(*), sum(v) FROM lv WHERE n > 1000'
]) q;

-- 3. count(DISTINCT) of integers, dates and long texts, alone and per group
SELECT same(q) FROM unnest(ARRAY[
	'SELECT g, count(DISTINCT t), count(DISTINCT n), count(DISTINCT d) FROM lv GROUP BY g',
	'SELECT count(DISTINCT t) FROM lv'
]) q;

-- 4. A sum of bigints beyond the range of one: numeric as the heap has it
CREATE TABLE big (g int, v bigint, w int);
INSERT INTO big SELECT i % 3, 9223372036854775000 - i, 2147480000 + i FROM generate_series(1, 2000) i;
CREATE INDEX big_col ON big USING sift_col (g, v, w);
SELECT same(q) FROM unnest(ARRAY[
	'SELECT g, sum(v), avg(v), min(v), max(v) FROM big GROUP BY g',
	'SELECT sum(v), sum(w), avg(w) FROM big'
]) q;

-- 5. GROUPING SETS, ROLLUP, CUBE and GROUPING()
SELECT same(q) FROM unnest(ARRAY[
	'SELECT g, count(*), sum(v) FROM lv GROUP BY ROLLUP (g)',
	'SELECT g, b, count(*), sum(n) FROM lv GROUP BY ROLLUP (g, b)',
	'SELECT g, b, count(*), grouping(g, b) FROM lv GROUP BY CUBE (g, b)',
	'SELECT g, t, count(*), sum(n), grouping(t) FROM lv GROUP BY GROUPING SETS ((g), (t), ())',
	'SELECT g, count(*) FROM lv WHERE n > 1000 GROUP BY GROUPING SETS ((g), ())',
	'SELECT g, d, count(DISTINCT t) FROM lv GROUP BY ROLLUP (g, d)',
	'SELECT g, count(*) FROM lv GROUP BY GROUPING SETS ((g), (g))',
	'SELECT g, count(*) FROM lv GROUP BY DISTINCT GROUPING SETS ((g), (g))'
]) q;

-- 6. The rows of this transaction: an INSERT, an UPDATE and a DELETE it has not committed are seen by it as the heap shows them, and a ROLLBACK leaves none
BEGIN;
INSERT INTO lv SELECT i, 3, 'tx', i, i, DATE '2023-01-01', TIMESTAMPTZ '2023-01-01', false, 7 FROM generate_series(40001, 40100) i;
UPDATE lv SET n = n + 1 WHERE id BETWEEN 100 AND 400;
DELETE FROM lv WHERE id BETWEEN 5000 AND 5200;
SELECT same(q) FROM unnest(ARRAY[
	'SELECT g, count(*), sum(n) FROM lv GROUP BY g',
	'SELECT count(*), sum(v), min(id), max(id) FROM lv'
]) q;
ROLLBACK;
SELECT same(q) FROM unnest(ARRAY[
	'SELECT g, count(*), sum(n) FROM lv GROUP BY g',
	'SELECT count(*), sum(v), min(id), max(id) FROM lv'
]) q;

-- 7. A statement the node does not take keeps its plan: an expression, a filter, an aggregate it has no state for, a column the index does not hold
SELECT same(q) FROM unnest(ARRAY[
	'SELECT g, count(*) FROM lv WHERE n BETWEEN 10 AND 20 AND g <> 3 GROUP BY g',
	'SELECT sum(n::int8 * 0 + id) FROM lv',
	'SELECT g, count(*) FILTER (WHERE n > 5) FROM lv GROUP BY g',
	'SELECT g, string_agg(t, '','') IS NOT NULL FROM lv GROUP BY g'
]) q;

-- 8. Groups beyond the memory of the statement are spilled to files by the hash of their key and merged a partition at a time: low and high cardinality, one key and several, a distinct set, ROLLUP
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SELECT same(q) FROM unnest(ARRAY[
	'SELECT id, count(*), sum(v), min(n), max(f), avg(v) FROM lv GROUP BY id',
	'SELECT id, count(DISTINCT t), count(*) FROM lv GROUP BY id',
	'SELECT id, g, count(*) FROM lv GROUP BY ROLLUP (id, g)',
	'SELECT d, ts, count(*), sum(n) FROM lv GROUP BY d, ts',
	'SELECT t, id, count(*) FROM lv GROUP BY t, id',
	'SELECT t, count(DISTINCT id), sum(n) FROM lv GROUP BY t',
	'SELECT g, count(*), sum(v) FROM lv GROUP BY g'
]) q;
RESET work_mem;
RESET hash_mem_multiplier;

-- 9. A table with an index of each format: the engine of each format plans its own index, and the answers agree whichever the planner keeps
SET sift.test_create_format1 = on;
CREATE TABLE mix (id int PRIMARY KEY, g int, n int);
INSERT INTO mix SELECT i, i % 5, i % 50 FROM generate_series(1, 5000) i;
CREATE INDEX mix_v1 ON mix USING sift_col (g, n, id);
SET sift.test_create_format1 = off;
CREATE INDEX mix_v2 ON mix USING sift_col (g, n, id);
SELECT sift._test_index_format('mix_v1') AS v1_format, sift._test_index_format('mix_v2') AS v2_format;
SELECT same(q) FROM unnest(ARRAY[
	'SELECT g, count(*), sum(n) FROM mix GROUP BY g',
	'SELECT g, count(*) FROM mix WHERE n > 20 GROUP BY ROLLUP (g)'
]) q;
DROP INDEX mix_v2;
SELECT same('SELECT g, count(*), sum(n) FROM mix GROUP BY g');

-- 10. A join of the facts with the pairs of a scope of the index (src/col/f2/agg/scope/scope.c): the cube of the scope key, read from the summary of the index without a row of the facts, when
-- the summary counts exactly what the heap shows the snapshot (the rows of its deletion vectors taken out); the join as the planner priced it, the child of the node, when it does not (a row
-- of this transaction or one not yet in the summary, a pair that changed, a window that is not months). The first word says the answers agree, the second that the plan is the node's, the third
-- where it read.
CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO tenant_scope SELECT m, 1 + (m * 31 + j * 13) % 40 FROM generate_series(1, 1500) m, generate_series(0, 5) j WHERE j < m % 7;
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, event_date date, kind int NOT NULL, value_num int NOT NULL, price float8, flag bool) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, CASE WHEN i % 17 = 0 THEN NULL ELSE 1 + (i * 7919) % 1500 END, timestamptz '2026-01-01' + i * interval '7 minutes', DATE '2026-01-01' + i / 100, i % 7,
	i % 1000, CASE WHEN i % 11 = 0 THEN NULL ELSE (i % 50) * 0.5 END, i % 3 = 0 FROM generate_series(1, 30000) i;
CREATE INDEX events_s ON events USING sift_col (id, dim_a_id, created_at, event_date, kind, value_num, price, flag) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)', track_changes = 'ctid');
ANALYZE events;
ANALYZE tenant_scope;
SET TimeZone = 'UTC';

CREATE FUNCTION scope_reads(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; read text := '-';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l LIKE '%Scope Read: %' THEN read := split_part(trim(substr(l, position('Scope Read: ' in l) + 12)), ' (', 1); END IF;
	END LOOP;
	RETURN read;
END $$;

CREATE FUNCTION scope_same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text; plan text := ''; l text; read text := '-';
BEGIN
	SET LOCAL sift.enable_agg = on;
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		plan := plan || l;
		IF l LIKE '%Scope Read: %' THEN read := trim(substr(l, position('Scope Read: ' in l) + 12)); END IF;
	END LOOP;
	EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO b;
	RETURN (CASE WHEN a IS DISTINCT FROM b THEN 'DIFFERENT' ELSE 'same' END) || ' ' || (CASE WHEN plan LIKE '%SiftAggV6Scope%' THEN 'scope' ELSE 'normal' END) || ' ' ||
		split_part(read, ' (', 1) || ' ' || q;
END $$;

CREATE FUNCTION scope_queries() RETURNS SETOF text LANGUAGE sql AS $$ SELECT unnest(ARRAY[
	'SELECT count(*), sum(e.value_num), min(e.kind), max(e.id), avg(e.value_num), count(e.price) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 7',
	'SELECT count(*), sum(e.id), sum(e.kind), min(e.value_num), max(e.value_num) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 40',
	'SELECT count(*), sum(e.id), min(e.id) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 999',
	'SELECT s.tenant_id, count(*), sum(e.value_num), min(e.id), max(e.kind), avg(e.kind) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id GROUP BY s.tenant_id',
	'SELECT s.tenant_id, count(*) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id GROUP BY s.tenant_id HAVING count(*) > 2100',
	'SELECT count(*), sum(e.value_num) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 7 AND e.created_at >= ''2026-02-01'' AND e.created_at < ''2026-04-01''',
	'SELECT count(*), max(e.id), avg(e.kind) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE e.created_at > ''2026-01-31 23:59:59.999999+00'' AND e.created_at <= ''2026-02-28 23:59:59.999999+00'' AND s.tenant_id = 12',
	'SELECT s.tenant_id, count(*), sum(e.value_num) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE e.created_at >= ''2026-03-01'' AND e.created_at < ''2026-05-01'' GROUP BY s.tenant_id',
	'SELECT count(*), sum(e.value_num) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 7 AND e.event_date >= DATE ''2026-02-01'' AND e.event_date < DATE ''2026-03-01''',
	'SELECT count(*) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 7 AND e.created_at >= ''2030-01-01'' AND e.created_at < ''2030-03-01''',
	'SELECT count(*) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 7 AND e.created_at >= ''2026-02-01'' AND e.created_at < ''2026-03-01'' AND e.created_at >= ''2026-02-05''',
	'SELECT count(*) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 7 AND e.created_at >= ''2026-02-03'' AND e.created_at < ''2026-03-01''',
	'SELECT count(*) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 7 AND e.created_at >= ''2026-02-01''',
	'SELECT count(*), sum(e.price) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 7',
	'SELECT count(*) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 7 AND e.kind = 3',
	'SELECT count(DISTINCT e.kind) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 7'
]) $$;

-- 10a. A clean index: whole key, every key, a key with no row, HAVING, windows of whole months (timestamptz and date, exclusive and inclusive bounds), and what the node does not take
SELECT scope_same(q) FROM scope_queries() q;
-- a window is a pair of absolute instants: month starts of UTC only where the session writes them in UTC (the windows spelled in another zone are read from the join, with the same answers)
SET TimeZone = 'America/New_York';
SELECT scope_same(q) FROM scope_queries() q;
SET TimeZone = 'UTC';
-- a parameter of a generic plan
PREPARE scope_p(int, timestamptz, timestamptz) AS SELECT count(*), sum(e.value_num) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = $1 AND e.created_at >= $2 AND e.created_at < $3;
SET plan_cache_mode = force_generic_plan;
SELECT scope_reads('EXECUTE scope_p(7, ''2026-02-01'', ''2026-03-01'')') AS generic_plan_read;
CREATE TEMP TABLE scope_p_on AS EXECUTE scope_p(7, '2026-02-01', '2026-03-01');
SET sift.enable_agg = off;
CREATE TEMP TABLE scope_p_off AS SELECT count(*), sum(e.value_num) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 7 AND e.created_at >= '2026-02-01' AND e.created_at < '2026-03-01';
RESET sift.enable_agg;
SELECT count(*) AS differing_rows FROM ((TABLE scope_p_on EXCEPT TABLE scope_p_off) UNION ALL (TABLE scope_p_off EXCEPT TABLE scope_p_on)) d;
RESET plan_cache_mode;
DROP TABLE scope_p_on, scope_p_off;
DEALLOCATE scope_p;

-- 10b. Deletes: the cubes of the summary less the rows the deletion vectors hold, before and after VACUUM
DELETE FROM events WHERE id % 29 = 0;
SELECT scope_same(q) FROM scope_queries() q WHERE q LIKE '%tenant_id = 7' OR q LIKE '%GROUP BY s.tenant_id';
VACUUM events;
SELECT scope_same(q) FROM scope_queries() q;
DELETE FROM events WHERE id % 31 = 0;
VACUUM events;
SELECT scope_same(q) FROM scope_queries() q WHERE q LIKE '%tenant_id = 7' OR q LIKE '%GROUP BY s.tenant_id';

-- 10c. Rows added and updated, then VACUUM
INSERT INTO events SELECT i, 1 + (i * 7919) % 1500, timestamptz '2026-03-01' + i * interval '9 minutes', DATE '2026-03-01', i % 7, i % 1000, 1, true FROM generate_series(50001, 50600) i;
UPDATE events SET value_num = value_num + 1, created_at = created_at + interval '1 day' WHERE id % 13 = 0;
SELECT scope_same(q) FROM scope_queries() q WHERE q LIKE '%tenant_id = 7' OR q LIKE '%GROUP BY s.tenant_id';
VACUUM events;
SELECT scope_same(q) FROM scope_queries() q;

-- 10d. The pairs change: the join answers until VACUUM builds the summary again
INSERT INTO tenant_scope SELECT m, 41 + m % 3 FROM generate_series(1, 300) m;
UPDATE tenant_scope SET tenant_id = tenant_id + 100 WHERE dim_a_id BETWEEN 400 AND 420 AND tenant_id < 10;
DELETE FROM tenant_scope WHERE dim_a_id BETWEEN 900 AND 960 AND tenant_id > 30;
SELECT scope_same(q) FROM scope_queries() q WHERE q LIKE '%tenant_id = 7' OR q LIKE '%GROUP BY s.tenant_id';
VACUUM events;
SELECT scope_same(q) FROM scope_queries() q;
SELECT scope_same('SELECT count(*), sum(e.value_num) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 42');

-- 10e. The rows of this transaction, and a rollback: the part its read of the index wrote stays, with an inserter that aborted, until a compaction drops it
BEGIN;
INSERT INTO events SELECT i, 1 + (i * 7919) % 1500, timestamptz '2026-03-01' + i * interval '9 minutes', DATE '2026-03-01', i % 7, i % 1000, 1, true FROM generate_series(50001, 50100) i;
SELECT scope_same(q) FROM scope_queries() q WHERE q LIKE '%tenant_id = 7' OR q LIKE '%GROUP BY s.tenant_id';
ROLLBACK;
BEGIN;
UPDATE events SET value_num = value_num + 1 WHERE id BETWEEN 100 AND 400;
SELECT scope_same(q) FROM scope_queries() q WHERE q LIKE '%tenant_id = 7';
ROLLBACK;
SELECT scope_same(q) FROM scope_queries() q WHERE q LIKE '%tenant_id = 7' OR q LIKE '%GROUP BY s.tenant_id';

-- 10f. A table whose index does not track changes, or a join that is not the scope's, keeps its plan
CREATE TABLE events_nt (LIKE events);
INSERT INTO events_nt SELECT * FROM events WHERE id % 5 = 0;
CREATE INDEX events_nt_s ON events_nt USING sift_col (id, dim_a_id, created_at, event_date, kind, value_num, price, flag) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
SELECT scope_same('SELECT count(*), sum(e.value_num) FROM events_nt e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 7');
SELECT scope_same('SELECT count(*) FROM events e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id AND s.tenant_id < e.kind WHERE s.tenant_id = 7');
SELECT scope_same('SELECT count(*) FROM events e LEFT JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 7');
DROP TABLE events_nt;
-- 10g. Numeric and float8 measures, the soft-delete test, the calendar month, a sub-select or materialized CTE for the pairs, and a key the grouping leaves to the constant
CREATE TABLE ev2 (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, event_date date, kind int NOT NULL, amount numeric(12,2), price float8, deleted_at timestamptz) WITH (autovacuum_enabled = false);
INSERT INTO ev2 SELECT i, 1 + (i * 7919) % 1500, timestamptz '2026-01-01' + i * interval '7 minutes', DATE '2026-01-01' + i / 100, i % 7, CASE WHEN i % 11 = 0 THEN NULL ELSE (i % 500) * 1.25 - 20 END,
	CASE WHEN i % 13 = 0 THEN NULL ELSE (i % 50) * 0.5 END, CASE WHEN i % 10 = 0 THEN timestamptz '2026-03-01' ELSE NULL END FROM generate_series(1, 30000) i;
CREATE INDEX ev2_s ON ev2 USING sift_col (id, dim_a_id, created_at, event_date, kind, amount, price, deleted_at) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)', track_changes = 'ctid');
ANALYZE ev2;
CREATE FUNCTION scope_queries2() RETURNS SETOF text LANGUAGE sql AS $$ SELECT unnest(ARRAY[
	'SELECT count(*), count(e.amount), sum(e.amount), avg(e.amount), min(e.amount), max(e.amount) FROM ev2 e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 7',
	'SELECT s.tenant_id, sum(e.amount), avg(e.amount) FROM ev2 e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id GROUP BY s.tenant_id',
	'SELECT count(*), sum(e.price), avg(e.price), min(e.price), max(e.price) FROM ev2 e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 12',
	'SELECT count(*), sum(e.amount), max(e.id) FROM ev2 e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 7 AND e.deleted_at IS NULL',
	'SELECT s.tenant_id, count(*), sum(e.price) FROM ev2 e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE e.deleted_at IS NULL GROUP BY s.tenant_id',
	'SELECT count(*), sum(e.amount) FROM ev2 e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 7 AND e.deleted_at IS NULL AND e.created_at >= ''2026-02-01'' AND e.created_at < ''2026-04-01''',
	'SELECT date_trunc(''month'', e.created_at), count(*), sum(e.amount) FROM ev2 e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 7 GROUP BY 1',
	'SELECT s.tenant_id, date_trunc(''month'', e.created_at) AS m, count(*), max(e.kind) FROM ev2 e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id GROUP BY 1, 2',
	'SELECT date_trunc(''month'', e.event_date), count(*), sum(e.price) FROM ev2 e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 7 AND e.deleted_at IS NULL GROUP BY 1',
	'SELECT date_trunc(''month'', e.created_at), count(*) FROM ev2 e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 7 AND e.created_at >= ''2026-02-01'' AND e.created_at < ''2026-04-01'' GROUP BY 1',
	'SELECT s.tenant_id, count(*), sum(e.amount) FROM ev2 e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 7 GROUP BY s.tenant_id',
	'SELECT s.tenant_id, count(*) FROM ev2 e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 999 GROUP BY s.tenant_id',
	'WITH ps AS MATERIALIZED (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 7) SELECT count(*), sum(e.amount) FROM ev2 e JOIN ps ON ps.dim_a_id = e.dim_a_id',
	'WITH ps AS MATERIALIZED (SELECT dim_a_id, tenant_id FROM tenant_scope) SELECT ps.tenant_id, count(*), sum(e.price) FROM ev2 e JOIN ps ON ps.dim_a_id = e.dim_a_id GROUP BY ps.tenant_id',
	'SELECT count(*) FROM ev2 e JOIN (SELECT dim_a_id FROM tenant_scope WHERE tenant_id = 7 OFFSET 0) ps ON ps.dim_a_id = e.dim_a_id',
	'SELECT count(*) FROM ev2 e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 7 AND e.deleted_at IS NOT NULL',
	'SELECT count(*), sum(e.amount) FROM ev2 e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 7 AND e.deleted_at IS NULL AND e.kind = 3'
]) $$;
SELECT scope_same(q) FROM scope_queries2() q;
SET TimeZone = 'America/New_York';
SELECT scope_same(q) FROM scope_queries2() q WHERE q LIKE '%date_trunc%';
SET TimeZone = 'UTC';
DELETE FROM ev2 WHERE id % 29 = 0;
UPDATE ev2 SET deleted_at = timestamptz '2026-04-01' WHERE id % 17 = 0;
SELECT scope_same(q) FROM scope_queries2() q WHERE q LIKE '%deleted_at IS NULL%' OR q LIKE '%tenant_id = 7';
VACUUM ev2;
SELECT scope_same(q) FROM scope_queries2() q;
-- a summary that lacks a tenth of the rows or less waits for the next flush (the join answers); one that lacks more is made again
INSERT INTO ev2 SELECT i, 1 + (i * 7919) % 1500, timestamptz '2026-01-01' + i * interval '7 minutes', DATE '2026-01-01' + i / 100, i % 7, CASE WHEN i % 11 = 0 THEN NULL ELSE (i % 500) * 1.25 - 20 END,
	CASE WHEN i % 13 = 0 THEN NULL ELSE (i % 50) * 0.5 END, CASE WHEN i % 10 = 0 THEN timestamptz '2026-03-01' ELSE NULL END FROM generate_series(30001, 34000) i;
VACUUM ev2;
SELECT scope_same(q) FROM scope_queries2() q;
-- the security of a table is the facts': a policy limits what the cubes would count, so the summary is not read
CREATE ROLE sift_rls_reader NOLOGIN;
GRANT SELECT ON ev2, tenant_scope TO sift_rls_reader;
ALTER TABLE ev2 ENABLE ROW LEVEL SECURITY;
CREATE POLICY ev2_kind ON ev2 FOR SELECT TO sift_rls_reader USING (kind < 3);
SET ROLE sift_rls_reader;
SELECT scope_same('SELECT count(*), sum(e.amount) FROM ev2 e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 7');
SELECT scope_same('SELECT s.tenant_id, count(*) FROM ev2 e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id GROUP BY s.tenant_id');
RESET ROLE;
SELECT scope_same('SELECT count(*), sum(e.amount) FROM ev2 e JOIN tenant_scope s ON s.dim_a_id = e.dim_a_id WHERE s.tenant_id = 7');
DROP POLICY ev2_kind ON ev2;
REVOKE ALL ON ev2, tenant_scope FROM sift_rls_reader;
DROP ROLE sift_rls_reader;
DROP FUNCTION scope_queries2();
DROP TABLE ev2;
-- numeric measures of a table that has no scope
CREATE TABLE nm (id int PRIMARY KEY, g int, amount numeric(12,2), w numeric(10,0), v int);
INSERT INTO nm SELECT i, i % 5, CASE WHEN i % 9 = 0 THEN NULL ELSE (i % 700) * 1.35 - 99 END, i % 77, i FROM generate_series(1, 20000) i;
CREATE INDEX nm_col ON nm USING sift_col (g, amount, w, v, id);
SELECT same(q) FROM unnest(ARRAY[
	'SELECT count(*), count(amount), sum(amount), avg(amount), min(amount), max(amount) FROM nm',
	'SELECT g, sum(amount), avg(amount), min(amount), max(amount), sum(w), max(w) FROM nm GROUP BY g',
	'SELECT g, sum(amount) FROM nm WHERE v > 15000 GROUP BY g',
	'SELECT g, sum(amount) FROM nm WHERE amount > 10 GROUP BY g',
	'SELECT amount, count(*) FROM nm GROUP BY amount'
]) q;
DROP TABLE nm;
DROP FUNCTION scope_same(text), scope_reads(text), scope_queries();
DROP TABLE events, tenant_scope;
RESET TimeZone;

DROP FUNCTION same(text);
DROP TABLE mix, big, lv;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
