-- The condition of a global-order walk is tested on the heap row of each entry (col_topn_gorder_filter). What that test relies on, case by case, against a copy of the table
-- without an index (the oracle): conditions that are not immutable comparisons are not tested there (a volatile function refuses the scan, an expression of a text column leaves the
-- units; a scalar sub-select that gives one row is a constant of the scan, tested like a number; a string against a constant is tested, col_topn_gorder_filter_text); NULL semantics of IS NULL, = NULL, a boolean that is NULL; the row tested is the version the scan's snapshot sees (own writes, a cursor opened before an update, a
-- rolled-back subtransaction, HOT chains); a scan that is rewound or restarted with another parameter (the budget starts again); parallel plans keep the units.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.async_maintenance = off;

CREATE TABLE tt AS SELECT i AS id, ((i::bigint * 7919) % 60001)::int AS k, ((i::bigint * 104729 + 13) % 10007)::int AS u, (i % 10) AS st,
	CASE i % 3 WHEN 0 THEN NULL WHEN 1 THEN true ELSE false END AS nb, CASE WHEN i % 50 = 0 THEN now() END AS gone, 'v' || (i % 7) AS tx, 0 AS pad
FROM generate_series(1, 60000) i;
CREATE INDEX tt_i ON tt USING sift_col (id, k, u, st, nb, gone, tx) WITH (order_columns = 'k', order_array_columns = 'k', write_buffer_limit = 256);
VACUUM ANALYZE tt;
CREATE TABLE tp AS SELECT * FROM tt;
ANALYZE tp;

CREATE FUNCTION global_order(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF l ~ 'Global Order: true' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;
CREATE FUNCTION same(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	EXECUTE format('SELECT string_agg(id::text, '','') FROM (%s) x', replace(q, '@', 'tt')) INTO a;
	EXECUTE format('SELECT string_agg(id::text, '','') FROM (%s) x', replace(q, '@', 'tp')) INTO b;
	IF a IS DISTINCT FROM b THEN RAISE NOTICE '% differs: %', q, left(a, 80); END IF;
	RETURN a IS NOT DISTINCT FROM b;
END $$;

CREATE TABLE tq (n int, q text);
INSERT INTO tq SELECT row_number() OVER (), q FROM unnest(ARRAY[
 -- NULL semantics
 'SELECT id FROM @ WHERE gone IS NULL ORDER BY k, id LIMIT 20', 'SELECT id FROM @ WHERE gone = NULL ORDER BY k, id LIMIT 20', 'SELECT id FROM @ WHERE NOT (gone IS NULL) ORDER BY k, id LIMIT 20',
 'SELECT id FROM @ WHERE gone IS NOT DISTINCT FROM NULL ORDER BY k, id LIMIT 20', 'SELECT id FROM @ WHERE gone < now() ORDER BY k, id LIMIT 20',
 'SELECT id FROM @ WHERE nb ORDER BY k, id LIMIT 20', 'SELECT id FROM @ WHERE NOT nb ORDER BY k, id LIMIT 20', 'SELECT id FROM @ WHERE nb IS NOT TRUE ORDER BY k, id LIMIT 20',
 'SELECT id FROM @ WHERE nb IS NULL ORDER BY k, id LIMIT 20', 'SELECT id FROM @ WHERE nb IS FALSE ORDER BY k, id LIMIT 20', 'SELECT id FROM @ WHERE nb = true ORDER BY k, id LIMIT 20',
 'SELECT id FROM @ WHERE COALESCE(nb, true) ORDER BY k, id LIMIT 20', 'SELECT id FROM @ WHERE u <> 5 AND gone IS NULL ORDER BY k, id LIMIT 20',
 -- not immutable comparisons of integers: volatile (the scan is refused), a scalar sub-select (a constant of the scan when the walk is cheaper than the units; the price of (SELECT 9000) keeps the sequential scan), a text column (a string against a constant is tested on the heap row when the walk is cheaper than the units)
 'SELECT id FROM @ WHERE gone IS NULL AND random() < 2 ORDER BY k, id LIMIT 20', 'SELECT id FROM @ WHERE u < (SELECT 9000) ORDER BY k, id LIMIT 20',
 'SELECT id FROM @ WHERE u < (SELECT max(u) FROM tp) - 100 ORDER BY k, id LIMIT 20', 'SELECT id FROM @ WHERE tx = ''v3'' ORDER BY k, id LIMIT 20',
 'SELECT id FROM @ WHERE tx < ''v5'' COLLATE "C" ORDER BY k, id LIMIT 20', 'SELECT id FROM @ WHERE tx LIKE ''v1%'' ORDER BY k, id LIMIT 20', 'SELECT id FROM @ WHERE lower(tx) = ''v2'' ORDER BY k, id LIMIT 20',
 'SELECT id FROM @ WHERE u + st < 9000 ORDER BY k, id LIMIT 20', 'SELECT id FROM @ WHERE gone IS NULL AND (u < 100 OR st = 3) ORDER BY k, id LIMIT 20',
 'SELECT id FROM @ WHERE gone IS NULL OR gone > now() ORDER BY k, id LIMIT 20']) q;
SELECT n, global_order(replace(q, '@', 'tt')) AS global_order, same(q) AS answers FROM tq ORDER BY n;

-- the snapshot: the row tested is the version the scan sees. Own writes, a cursor opened before an update, a rolled-back subtransaction
CREATE FUNCTION page(t text, flt text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT string_agg(id::text, '','' ORDER BY k, id) FROM (SELECT id, k FROM %s WHERE %s ORDER BY k, id LIMIT 30) x', t, flt) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION agrees(flt text) RETURNS boolean LANGUAGE sql AS $$ SELECT page('tt', flt) IS NOT DISTINCT FROM page('tp', flt) $$;

BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT count(*) > 0 AS snapshot_taken FROM tt;
UPDATE tt SET gone = now() WHERE id % 3 = 0 AND k < 3000;
UPDATE tp SET gone = now() WHERE id % 3 = 0 AND k < 3000;
SELECT agrees('gone IS NULL') AS own_writes_flag, agrees('nb') AS own_writes_bool;
SAVEPOINT s;
UPDATE tt SET gone = NULL, k = k + 5000 WHERE k < 2000;
UPDATE tp SET gone = NULL, k = k + 5000 WHERE k < 2000;
SELECT agrees('gone IS NULL') AS in_subtransaction;
ROLLBACK TO s;
SELECT agrees('gone IS NULL') AS after_rollback_to;
COMMIT;

CREATE TABLE tcur (n int, a text, b text);
BEGIN;
DECLARE c1 CURSOR FOR SELECT id FROM tt WHERE gone IS NULL AND u < 9900 ORDER BY k, id LIMIT 40;
DECLARE c2 CURSOR FOR SELECT id FROM tp WHERE gone IS NULL AND u < 9900 ORDER BY k, id LIMIT 40;
FETCH 5 FROM c1;
FETCH 5 FROM c2;
UPDATE tt SET gone = now() WHERE k < 4000;
UPDATE tp SET gone = now() WHERE k < 4000;
FETCH 5 FROM c1;
FETCH 5 FROM c2;
COMMIT;
SELECT agrees('gone IS NULL AND u < 9900') AS after_cursor_update;

-- HOT chains: pad is not in the index, so the update leaves the entry and the visible version is another one of the chain
UPDATE tt SET pad = pad + 1;
UPDATE tp SET pad = pad + 1;
SELECT agrees('gone IS NULL') AS hot_chains, agrees('nb IS NOT TRUE') AS hot_chains_bool;
UPDATE tt SET pad = pad + 1 WHERE id % 2 = 0;
UPDATE tp SET pad = pad + 1 WHERE id % 2 = 0;
SELECT agrees('gone IS NULL') AS hot_chains_again;

-- a scan that is rewound (the rows replay) and restarted with another parameter (the pass rate, the budget and the queue start again)
SET enable_material = off; SET enable_hashjoin = off; SET enable_mergejoin = off; SET enable_nestloop = on;
SELECT (SELECT string_agg(s.id::text, ',' ORDER BY g, s.k, s.id) FROM generate_series(1, 3) g CROSS JOIN LATERAL (SELECT id, k FROM tt WHERE gone IS NULL ORDER BY k, id LIMIT 7) s)
	IS NOT DISTINCT FROM (SELECT string_agg(s.id::text, ',' ORDER BY g, s.k, s.id) FROM generate_series(1, 3) g CROSS JOIN LATERAL (SELECT id, k FROM tp WHERE gone IS NULL ORDER BY k, id LIMIT 7) s) AS restarted;
SELECT (SELECT string_agg(s.id::text, ',' ORDER BY g, s.k, s.id) FROM generate_series(1, 4) g CROSS JOIN LATERAL (SELECT id, k FROM tt WHERE u < g * g * 600 ORDER BY k, id LIMIT 20) s)
	IS NOT DISTINCT FROM (SELECT string_agg(s.id::text, ',' ORDER BY g, s.k, s.id) FROM generate_series(1, 4) g CROSS JOIN LATERAL (SELECT id, k FROM tp WHERE u < g * g * 600 ORDER BY k, id LIMIT 20) s) AS parameter_changes_pass_rate;
SELECT (SELECT string_agg(s.id::text, ',' ORDER BY g, s.k, s.id) FROM generate_series(1, 3) g CROSS JOIN (SELECT id, k FROM tt WHERE gone IS NULL ORDER BY k, id LIMIT 7) s)
	IS NOT DISTINCT FROM (SELECT string_agg(s.id::text, ',' ORDER BY g, s.k, s.id) FROM generate_series(1, 3) g CROSS JOIN (SELECT id, k FROM tp WHERE gone IS NULL ORDER BY k, id LIMIT 7) s) AS rewound;
RESET enable_material; RESET enable_hashjoin; RESET enable_mergejoin; RESET enable_nestloop;

-- parallel plans: the walk is one process's (the scan is not parallel), the rows are the same
SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0; SET max_parallel_workers_per_gather = 4; SET debug_parallel_query = on;
SELECT n, same(q) AS answers_parallel FROM tq WHERE n IN (1, 6, 7, 12, 13, 15, 20) ORDER BY n;
SELECT global_order('SELECT id FROM tt WHERE gone IS NULL ORDER BY k, id LIMIT 20') AS parallel_global_order;
RESET debug_parallel_query; RESET max_parallel_workers_per_gather; RESET parallel_setup_cost; RESET parallel_tuple_cost; RESET min_parallel_table_scan_size;

-- after rows were written and promoted
INSERT INTO tt SELECT 60000 + i, i % 90, i % 10007, i % 10, CASE i % 3 WHEN 0 THEN NULL ELSE i % 2 = 0 END, CASE WHEN i % 3 = 0 THEN now() END, 'v' || (i % 7), 0 FROM generate_series(1, 300) i;
INSERT INTO tp SELECT 60000 + i, i % 90, i % 10007, i % 10, CASE i % 3 WHEN 0 THEN NULL ELSE i % 2 = 0 END, CASE WHEN i % 3 = 0 THEN now() END, 'v' || (i % 7), 0 FROM generate_series(1, 300) i;
SELECT sift.col_promote('tt_i') >= 0 AS promoted;
SELECT n, same(q) AS answers FROM tq ORDER BY n;

DROP TABLE tq, tcur, tt, tp;
DROP FUNCTION global_order(text), same(text), page(text, text), agrees(text);
DROP EXTENSION pg_sift CASCADE;
