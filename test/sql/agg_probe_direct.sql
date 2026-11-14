-- A correlated lookup child (a LEFT JOIN LATERAL .. LIMIT 1 into a table with a btree) is probed by the aggregate scan in the btree itself, the keys of a chunk together
-- (combo/probe.c), instead of running the child's plan for each key. Each case runs the query three ways, the plan of the database (sift.enable_agg off), the scan with
-- the child's plan for every key (sift.agg_probe_direct off) and the scan with the direct probe, and prints how many rows differ from the direct probe's; `direct` says the
-- probe ran (shapes and indexes it does not take leave the keys to the child). The lookup tables carry what the index must get right: dead versions that stay in the
-- index until a VACUUM, HOT chains, duplicate keys (the first visible entry in index order is the match), NULL keys, rows of the transaction itself, pages that were emptied.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 100; SET random_page_cost = 4;
-- the children are priced by what a probe takes (cost/join.c child_probe_ops): the 900 rows of a table are cheaper loaded, and the statements check the child runs
SET sift.agg_force = on;
\ir test/agg_probe_helpers.sql
-- one lookup table per key type and opclass: the key of row g, and the key an item builds (the same function of its number n)
CREATE TABLE types (ord serial, name text, coltype text, idxdef text, rowkey text, itemkey text, cond text);
INSERT INTO types (name, coltype, idxdef, rowkey, itemkey) VALUES
  ('int2',            'int2',        '(k)',                    'g::int2',                                                  'o.n::int2'),
  ('int4',            'int4',        '(k)',                    'g::int4',                                                  'o.n::int4'),
  ('int8',            'int8',        '(k)',                    'g::int8',                                                  'o.n::int8'),
  ('int8 desc',       'int8',        '(k DESC)',               'g::int8',                                                  'o.n::int8'),
  ('int4 nulls first','int4',        '(k NULLS FIRST)',        'g::int4',                                                  'o.n::int4'),
  ('date',            'date',        '(k)',                    'date ''2025-01-01'' + g',                                  'date ''2025-01-01'' + o.n::int'),
  ('timestamptz',     'timestamptz', '(k)',                    'timestamptz ''2025-01-01 00:00+00'' + g * interval ''1 hour''', 'timestamptz ''2025-01-01 00:00+00'' + o.n::int * interval ''1 hour'''),
  -- negative integers and dates before 2000 (stored as negative integers), the infinities of date and timestamps, and the uuid that is compared as its 16 bytes, ascending, descending and with the NULLs first
  ('int4 negative',   'int4',        '(k)',                    '(g - 150)::int4',                                          '(o.n::int - 150)::int4'),
  ('int8 negative desc', 'int8',     '(k DESC)',               '(g - 150)::int8 * 100000000000',                           '(o.n::int - 150)::int8 * 100000000000'),
  ('date negative',   'date',        '(k)',                    'date ''1999-06-01'' + g',                                  'date ''1999-06-01'' + o.n::int'),
  ('date infinity',   'date',        '(k DESC NULLS FIRST)',   'CASE WHEN g % 50 = 0 THEN ''infinity''::date WHEN g % 50 = 1 THEN ''-infinity''::date ELSE date ''2025-01-01'' + g END', 'CASE WHEN o.n::int % 50 = 0 THEN ''infinity''::date WHEN o.n::int % 50 = 1 THEN ''-infinity''::date ELSE date ''2025-01-01'' + o.n::int END'),
  ('timestamp',       'timestamp',   '(k)',                    'timestamp ''2025-01-01 00:00'' + (g - 150) * interval ''1 hour''', 'timestamp ''2025-01-01 00:00'' + (o.n::int - 150) * interval ''1 hour'''),
  ('timestamp infinity', 'timestamp', '(k)',                   'CASE WHEN g % 50 = 0 THEN ''infinity''::timestamp WHEN g % 50 = 1 THEN ''-infinity''::timestamp ELSE timestamp ''1999-12-30 00:00'' + g * interval ''1 hour'' END', 'CASE WHEN o.n::int % 50 = 0 THEN ''infinity''::timestamp WHEN o.n::int % 50 = 1 THEN ''-infinity''::timestamp ELSE timestamp ''1999-12-30 00:00'' + o.n::int * interval ''1 hour'' END'),
  ('timestamptz infinity desc', 'timestamptz', '(k DESC)',     'CASE WHEN g % 50 = 0 THEN ''infinity''::timestamptz WHEN g % 50 = 1 THEN ''-infinity''::timestamptz ELSE timestamptz ''2000-01-01 00:00+00'' + (g - 150) * interval ''1 hour'' END', 'CASE WHEN o.n::int % 50 = 0 THEN ''infinity''::timestamptz WHEN o.n::int % 50 = 1 THEN ''-infinity''::timestamptz ELSE timestamptz ''2000-01-01 00:00+00'' + (o.n::int - 150) * interval ''1 hour'' END'),
  ('uuid',            'uuid',        '(k)',                    'md5(g::text)::uuid',                                       'md5(d.n::text)::uuid'),
  ('uuid desc',       'uuid',        '(k DESC)',               'md5(g::text)::uuid',                                       'md5(d.n::text)::uuid'),
  ('uuid nulls first','uuid',        '(k NULLS FIRST)',        'CASE WHEN g % 40 = 0 THEN NULL ELSE md5(g::text)::uuid END', 'md5(d.n::text)::uuid'),
  ('numeric',         'numeric',     '(k)',                    'g::numeric',                                               'o.n::numeric'),
  ('float8',          'float8',      '(k)',                    'g::float8',                                                'o.n'),
  ('interval',        'interval',    '(k)',                    'g * interval ''1 minute''',                                'o.n::int * interval ''1 minute'''),
  ('bool',            'bool',        '(k)',                    'g % 2 = 0',                                                'o.n::int % 2 = 0'),
  ('bytea',           'bytea',       '(k)',                    'convert_to(''t'' || g, ''UTF8'')',                         'convert_to(''t'' || o.n::int, ''UTF8'')'),
  ('text',            'text',        '(k)',                    '''t'' || g',                                               '''t'' || o.n::int'),
  ('text C',          'text',        '(k COLLATE "C")',        '''t'' || g',                                               '''t'' || o.n::int'),
  ('text pattern',    'text',        '(k text_pattern_ops)',   '''t'' || g',                                               '''t'' || o.n::int'),
  ('varchar',         'varchar(20)', '(k)',                    '''t'' || g',                                               '(''t'' || o.n::int)::varchar'),
  ('bpchar',          'char(8)',     '(k)',                    '(''t'' || g)::char(8)',                                    '(''t'' || o.n::int)::char(8)'),
  ('name',            'name',        '(k)',                    '(''t'' || g)::name',                                       '(''t'' || o.n::int)::name'),
  ('int4 array',      'int4[]',      '(k)',                    'ARRAY[g, g % 3]',                                          'ARRAY[o.n::int4, o.n::int4 % 3]'),
  ('text array',      'text[]',      '(k)',                    'ARRAY[''t'' || g]',                                        'ARRAY[''t'' || o.n::int]'),
  ('int4 expression', 'int4',        '((k * 2))',              'g::int4',                                                  'o.n::int4'),
  ('partial',         'int4',        '(k) WHERE v IS NOT NULL','g::int4',                                                  'o.n::int4'),
  ('include',         'int4',        '(k) INCLUDE (v)',        'g::int4',                                                  'o.n::int4'),
  ('hash',            'int4',        'USING hash (k)',         'g::int4',                                                  'o.n::int4'),
  ('brin',            'int4',        'USING brin (k)',         'g::int4',                                                  'o.n::int4'),
  -- an integer column compared with the key of another integer type, and keys beyond the range of the column (no entry equals them)
  ('int2 key int4',   'int2',        '(k)',                    'g::int2',                                                  'o.n::int4 + CASE WHEN o.n::int % 4 = 0 THEN 65536 ELSE 0 END'),
  ('int4 key int8',   'int4',        '(k)',                    'g::int4',                                                  'o.n::int8 + CASE WHEN o.n::int % 4 = 0 THEN 4294967296 ELSE 0 END'),
  ('int8 key int2',   'int8',        '(k)',                    'g::int8',                                                  'o.n::int2'),
  ('int8 desc key int4', 'int8',     '(k DESC)',               'g::int8',                                                  'o.n::int4');
DO $$
DECLARE t record;
BEGIN
  FOR t IN SELECT * FROM types LOOP
    EXECUTE format('CREATE TABLE lk_%s (k %s, v text)', t.ord, t.coltype);
    EXECUTE format('INSERT INTO lk_%s SELECT %s, ''v'' || g FROM generate_series(0, 299) g WHERE g %% 7 <> 0', t.ord, t.rowkey);
    EXECUTE format('CREATE INDEX ON lk_%s %s', t.ord, t.idxdef);
  END LOOP;
END $$;
-- the conditions of the indexes that are not on k alone
UPDATE types SET cond = 'k * 2 = (' || itemkey || ') * 2' WHERE name = 'int4 expression';
UPDATE types SET cond = 'k = (' || itemkey || ') AND v IS NOT NULL' WHERE name = 'partial';
UPDATE types SET cond = 'k COLLATE "C" = (' || itemkey || ')' WHERE name = 'text C';
UPDATE types SET cond = 'k = (' || itemkey || ')' WHERE cond IS NULL;
VACUUM (FREEZE, ANALYZE) entries;
ANALYZE;
-- the key of the uuid cases is built from a column (md5 is not evaluated on the items of a document): a table of numbers, one in nine NULL
CREATE TABLE ud (id int, n int);
INSERT INTO ud SELECT g, CASE WHEN g % 9 = 0 THEN NULL ELSE (g * 7) % 330 END FROM generate_series(1, 2000) g;
CREATE INDEX ud_col ON ud USING sift_col (id, n);
VACUUM (FREEZE, ANALYZE) ud;
CREATE FUNCTION case_query_ud(tab text, cond text) RETURNS text LANGUAGE sql AS $$
  SELECT format($q$SELECT m.v, count(*) AS n, sum(d.n) AS total FROM ud d LEFT JOIN LATERAL (SELECT v FROM %s WHERE %s LIMIT 1) m ON true GROUP BY 1$q$, tab, cond) $$;
CREATE FUNCTION types_query(t types) RETURNS text LANGUAGE sql AS $$
  SELECT CASE WHEN t.name LIKE 'uuid%' THEN case_query_ud('lk_' || t.ord, t.cond) ELSE case_query('lk_' || t.ord, t.cond) END $$;
SELECT t.name, p.* FROM types t, LATERAL probe_check(types_query(t)) p ORDER BY t.ord;

-- the integers, dates, timestamps, uuids and strings of a bytewise order are compared in the probe itself, never through the opclass's order function (sift._test_probe_stats counts those
-- calls); the other types are
CREATE EXTENSION pg_sift_test;
CREATE FUNCTION order_function_calls(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
  SET LOCAL sift.enable_agg = on;
  PERFORM sift._test_probe_stats(true);
  EXECUTE 'SELECT count(*) FROM (' || q || ') q';
  SELECT generic_compares INTO n FROM sift._test_probe_stats(true);
  RETURN n > 0;
END $$;
SELECT t.name, order_function_calls(types_query(t)) AS order_function_called FROM types t
WHERE t.name IN ('int2', 'int4', 'int8', 'int8 desc', 'int4 nulls first', 'date', 'timestamptz', 'text C', 'text pattern', 'int4 negative', 'int8 negative desc', 'date negative', 'date infinity',
                 'timestamp', 'timestamp infinity', 'timestamptz infinity desc', 'uuid', 'uuid desc', 'uuid nulls first', 'int2 key int4', 'int4 key int8', 'int8 key int2', 'numeric', 'float8', 'interval')
ORDER BY t.ord;

-- duplicate keys, versions and dead entries. Three rows per key (the match of a LIMIT 1 is the first visible entry of the key in index order); an index on w too, so that
-- a change of w is not a HOT update and leaves the old entry of k in the index until a VACUUM
CREATE TABLE lk_dup (k int, w int, v text);
INSERT INTO lk_dup SELECT g, r, 'v' || g || '.' || r FROM generate_series(0, 299) g, generate_series(1, 3) r WHERE g % 7 <> 0 ORDER BY r, g;
CREATE INDEX lk_dup_k ON lk_dup (k);
CREATE INDEX lk_dup_w ON lk_dup (w);
CREATE INDEX lk_dup_kw ON lk_dup (k, w);
ANALYZE lk_dup;
SELECT * FROM step('three rows per key', 'lk_dup', 'k = o.n::int');
UPDATE lk_dup SET v = v || 'h' WHERE k % 5 = 0;
UPDATE lk_dup SET v = v || 'h' WHERE k % 5 = 0;
UPDATE lk_dup SET v = v || 'h' WHERE k % 5 = 0 AND w = 1;
SELECT * FROM step('hot chains', 'lk_dup', 'k = o.n::int');
UPDATE lk_dup SET w = w + 10 WHERE k % 3 = 0;
SELECT * FROM step('entries of dead versions', 'lk_dup', 'k = o.n::int');
SELECT * FROM step('entries marked dead', 'lk_dup', 'k = o.n::int');
DELETE FROM lk_dup WHERE k % 11 = 0 AND w = 1;
DELETE FROM lk_dup WHERE k % 13 = 0 AND w <> 3;
SELECT * FROM step('deleted rows', 'lk_dup', 'k = o.n::int');
INSERT INTO lk_dup SELECT g, 0, 'late' || g FROM generate_series(0, 299, 4) g;
SELECT * FROM step('inserted rows', 'lk_dup', 'k = o.n::int');
SELECT * FROM step('all of the key', 'lk_dup', 'k = o.n::int', '');
-- a child that gives every match (OFFSET 0 keeps it from being pulled up into the join): all the visible entries of the key in index order; and the EXISTS form, a constant for the first match
SELECT * FROM step('every match of the key', 'lk_dup', 'k = o.n::int', 'OFFSET 0');
SELECT * FROM step('every match under a filter', 'lk_dup', 'k = o.n::int AND w <> 2', 'OFFSET 0');
SELECT * FROM step_io('exists form', 'lk_dup', 'k = o.n::int', 'LIMIT 1', '1');
SELECT * FROM step_io('exists form under a filter', 'lk_dup', 'k = o.n::int AND w = 3', 'LIMIT 1', '1');
SELECT * FROM step('two of the key', 'lk_dup', 'k = o.n::int', 'LIMIT 2');
SELECT * FROM step('first by w', 'lk_dup', 'k = o.n::int', 'ORDER BY w LIMIT 1');
SELECT * FROM step('last by w', 'lk_dup', 'k = o.n::int', 'ORDER BY w DESC LIMIT 1');
SELECT * FROM step('filter on the row', 'lk_dup', 'k = o.n::int AND w <> 2');
SELECT * FROM step('filter on a value of the outer row', 'lk_dup', 'k = o.n::int AND w <> o.score');
SELECT * FROM step('range condition on the row', 'lk_dup', 'k = o.n::int AND w > 2');
SELECT * FROM step('filter on the outer row', 'lk_dup', 'k = o.n::int AND e.dim_b_id % 2 = 0');
SELECT * FROM step('key of another type', 'lk_dup', 'k::int8 = o.n::int8');
SELECT * FROM step('key out of range', 'lk_dup', 'k = o.n::int + 1000');
BEGIN;
UPDATE lk_dup SET v = v || 't' WHERE k % 4 = 0;
DELETE FROM lk_dup WHERE k % 9 = 0;
INSERT INTO lk_dup SELECT g, -1, 'own' || g FROM generate_series(0, 299, 3) g;
SELECT * FROM step('rows of the transaction', 'lk_dup', 'k = o.n::int');
SELECT * FROM step('every match of the transaction', 'lk_dup', 'k = o.n::int', 'OFFSET 0');
SAVEPOINT s1;
UPDATE lk_dup SET w = w + 100 WHERE k % 2 = 0;
SELECT * FROM step('rows of a savepoint', 'lk_dup', 'k = o.n::int');
ROLLBACK TO s1;
SELECT * FROM step('after rollback to savepoint', 'lk_dup', 'k = o.n::int');
ROLLBACK;
SELECT * FROM step('after rollback', 'lk_dup', 'k = o.n::int');
VACUUM lk_dup;
SELECT * FROM step('after vacuum', 'lk_dup', 'k = o.n::int');
DELETE FROM lk_dup WHERE k % 10 <> 0;
SELECT * FROM step('most rows deleted', 'lk_dup', 'k = o.n::int');
VACUUM lk_dup;
SELECT * FROM step('emptied pages vacuumed', 'lk_dup', 'k = o.n::int');
INSERT INTO lk_dup SELECT g, 5, 'again' || g FROM generate_series(0, 299) g;
SELECT * FROM step('reinserted', 'lk_dup', 'k = o.n::int');
REINDEX TABLE lk_dup;
SELECT * FROM step('reindexed', 'lk_dup', 'k = o.n::int');
-- the index scan takes the predicate locks of a serializable transaction, which the probe does not: such a transaction keeps the child's plan
BEGIN ISOLATION LEVEL SERIALIZABLE;
SELECT * FROM step('serializable transaction', 'lk_dup', 'k = o.n::int');
COMMIT;

-- a key whose dead versions fill several pages of the index: the match is after the pages of its dead entries (no deduplication, wide keys: a few dozen entries a page)
CREATE TABLE lk_run (k text, w int, v text);
INSERT INTO lk_run SELECT 't' || g || repeat('x', 200), 0, 'v' || g FROM generate_series(0, 299) g WHERE g % 7 <> 0;
CREATE INDEX lk_run_k ON lk_run (k) WITH (deduplicate_items = off);
CREATE INDEX lk_run_w ON lk_run (w);
DO $$ BEGIN FOR i IN 1..120 LOOP UPDATE lk_run SET w = i WHERE k LIKE 't1%' OR k LIKE 't2%'; END LOOP; END $$;
ANALYZE lk_run;
SET enable_bitmapscan = off;
SELECT * FROM step('dead versions over pages', 'lk_run', $c$k = 't' || o.n::int || repeat('x', 200)$c$);
SELECT * FROM step('dead entries marked', 'lk_run', $c$k = 't' || o.n::int || repeat('x', 200)$c$);
RESET enable_bitmapscan;
SELECT pg_relation_size('lk_run_k') / current_setting('block_size')::int > 500 AS entries_fill_pages;

-- the live row is the first entry of its key in index order and the dead versions come after it (rows inserted and deleted later have larger TIDs), over pages of entries:
-- the search must not pass the first page of the key's entries
CREATE TABLE lk_back (k text, w int, v text);
INSERT INTO lk_back SELECT 't' || g || repeat('x', 200), 0, 'v' || g FROM generate_series(0, 299) g WHERE g % 7 <> 0;
CREATE INDEX lk_back_k ON lk_back (k) WITH (deduplicate_items = off);
DO $$ BEGIN FOR i IN 1..120 LOOP
  INSERT INTO lk_back SELECT k, i, 'dead' FROM lk_back WHERE w = 0 AND (k LIKE 't1%' OR k LIKE 't2%');
  DELETE FROM lk_back WHERE w = i;
END LOOP; END $$;
ANALYZE lk_back;
SET enable_bitmapscan = off;
SELECT * FROM step('live row before dead versions', 'lk_back', $c$k = 't' || o.n::int || repeat('x', 200)$c$);
SELECT * FROM step('live row before dead entries marked', 'lk_back', $c$k = 't' || o.n::int || repeat('x', 200)$c$);
RESET enable_bitmapscan;
-- an Index Only Scan child takes the columns from the index tuple and asks the heap only for the tuples of the pages that are not all-visible (x is not indexed: its updates are HOT)
CREATE TABLE lk_io (k int, v text, w int, x int);
INSERT INTO lk_io SELECT g, 'v' || g || '.' || r, r, 0 FROM generate_series(0, 299) g, generate_series(1, 3) r WHERE g % 7 <> 0 ORDER BY r, g;
CREATE INDEX lk_io_kv ON lk_io (k, v);
CREATE INDEX lk_io_w ON lk_io (w);
VACUUM (FREEZE, ANALYZE) lk_io;
SET enable_bitmapscan = off;
SELECT * FROM step_io('all pages all-visible', 'lk_io', 'k = o.n::int');
SELECT * FROM step_io('all rows of the key', 'lk_io', 'k = o.n::int', 'OFFSET 0');
SELECT * FROM step_io('exists form', 'lk_io', 'k = o.n::int', 'LIMIT 1', '1');
SELECT * FROM step_io('filter on an index column', 'lk_io', $c$k = o.n::int AND v <> 'v1.1'$c$);
UPDATE lk_io SET x = 1 WHERE k % 5 = 0;
SELECT * FROM step_io('pages not all-visible, HOT chains', 'lk_io', 'k = o.n::int');
SELECT * FROM step_io('all rows, HOT chains', 'lk_io', 'k = o.n::int', 'OFFSET 0');
UPDATE lk_io SET v = v || 'u' WHERE k % 3 = 0;
SELECT * FROM step_io('entries of dead versions', 'lk_io', 'k = o.n::int');
SELECT * FROM step_io('entries marked dead', 'lk_io', 'k = o.n::int');
SELECT * FROM step_io('all rows, dead versions', 'lk_io', 'k = o.n::int', 'OFFSET 0');
-- the child's filter on an index column rejects an entry before the heap is asked about it (a row of a dead version no more than a visible one); and keys of another integer type
SELECT * FROM step_io('filter on an index column, dead versions', 'lk_io', $c$k = o.n::int AND v ~ 'u$'$c$);
SELECT * FROM step_io('filter rejects every entry', 'lk_io', $c$k = o.n::int AND v = 'none'$c$);
SELECT * FROM step_io('key of a wider integer type', 'lk_io', 'k = o.n::int8');
SELECT * FROM step_io('key of a narrower integer type', 'lk_io', 'k = o.n::int2');
DELETE FROM lk_io WHERE k % 11 = 0 AND w = 1;
SELECT * FROM step_io('deleted rows', 'lk_io', 'k = o.n::int');
INSERT INTO lk_io SELECT g, 'late' || g, 0, 0 FROM generate_series(0, 299, 4) g;
SELECT * FROM step_io('inserted rows', 'lk_io', 'k = o.n::int');
BEGIN;
UPDATE lk_io SET v = v || 't' WHERE k % 4 = 0;
DELETE FROM lk_io WHERE k % 9 = 0;
INSERT INTO lk_io SELECT g, 'own' || g, -1, 0 FROM generate_series(0, 299, 3) g;
SELECT * FROM step_io('rows of the transaction', 'lk_io', 'k = o.n::int');
SELECT * FROM step_io('all rows of the transaction', 'lk_io', 'k = o.n::int', 'OFFSET 0');
ROLLBACK;
SELECT * FROM step_io('after rollback', 'lk_io', 'k = o.n::int');
VACUUM lk_io;
SELECT * FROM step_io('after vacuum, all-visible again', 'lk_io', 'k = o.n::int');
SELECT * FROM step_io('all rows after vacuum', 'lk_io', 'k = o.n::int', 'OFFSET 0');
RESET enable_bitmapscan;
-- INCLUDE columns come from the index tuple as well
CREATE TABLE lk_inc (k int, v text);
INSERT INTO lk_inc SELECT g, 'v' || g FROM generate_series(0, 299) g WHERE g % 7 <> 0;
CREATE INDEX lk_inc_k ON lk_inc (k) INCLUDE (v);
VACUUM (FREEZE, ANALYZE) lk_inc;
SELECT * FROM step_io('include column', 'lk_inc', 'k = o.n::int');
-- A NULL in an INCLUDE column (or a later key column) is not among the columns a probe compares: the compare keeps its fast way, and only a NULL in a compared column
-- reads the columns one by one (sift._test_probe_stats, slow_compares). The covering index is what the planner takes for an Index Only Scan when it holds every column the
-- child reads, the column inside an indexed expression included: a lookup child that reads (scope, mapped value) through coalesce(scope, '{}') is answered from the
-- visibility map once the table is vacuumed.
CREATE TABLE lk_incn (k int, a text, b int, v text);
INSERT INTO lk_incn SELECT g, CASE WHEN g % 3 = 0 THEN 'a' || g END, CASE WHEN g % 5 = 0 THEN g END, 'v' || g FROM generate_series(0, 299) g WHERE g % 7 <> 0;
CREATE INDEX lk_incn_k ON lk_incn (k) INCLUDE (a, b, v);
CREATE TABLE lk_nullk (k int, a text, v text);
INSERT INTO lk_nullk SELECT CASE WHEN g % 2 = 0 THEN NULL ELSE g END, CASE WHEN g % 3 = 0 THEN 'a' || g END, 'v' || g FROM generate_series(0, 299) g WHERE g % 7 <> 0;
CREATE INDEX lk_nullk_k ON lk_nullk (k NULLS FIRST) INCLUDE (a, v);
CREATE TABLE lk_incx (s int[], t text, v text);
INSERT INTO lk_incx SELECT CASE WHEN g % 40 = 0 THEN ARRAY[g] END, 't' || g, 'v' || g FROM generate_series(0, 299) g WHERE g % 7 <> 0;
CREATE INDEX lk_incx_k ON lk_incx ((coalesce(s, '{}'::int[])), t) INCLUDE (s, v);
VACUUM (FREEZE, ANALYZE) lk_incn, lk_nullk, lk_incx;
SELECT * FROM step_io('include columns with NULLs', 'lk_incn', 'k = o.n::int', 'LIMIT 1', 'a');
SELECT * FROM step_io('include column read when NULL', 'lk_incn', 'k = o.n::int', 'LIMIT 1', 'b');
SELECT * FROM step_io('NULL in a key column', 'lk_nullk', 'k = o.n::int', 'LIMIT 1', 'a');
SELECT * FROM step_io('expression key, its column included', 'lk_incx', 'coalesce(s, ''{}''::int[]) = ''{}'' AND t = ''t'' || o.n::int', 'LIMIT 1', 'v');
CREATE FUNCTION slow_compares(q text) RETURNS TABLE (compared bool, slow bigint) LANGUAGE plpgsql AS $$
DECLARE s record;
BEGIN
  SET LOCAL sift.enable_agg = on;
  PERFORM sift._test_probe_stats(true);
  EXECUTE 'SELECT count(*) FROM (' || q || ') q';
  SELECT tuple_compares, slow_compares INTO s FROM sift._test_probe_stats(true);
  compared := s.tuple_compares > 0;
  slow := s.slow_compares;
  RETURN NEXT;
END $$;
SELECT 'NULL only in INCLUDE columns' AS lookup, * FROM slow_compares(case_query_col('lk_incn', 'k = o.n::int', 'LIMIT 1', 'a'));
SELECT 'NULL in the compared column' AS lookup, compared, slow > 0 AS slow_path FROM slow_compares(case_query_col('lk_nullk', 'k = o.n::int', 'LIMIT 1', 'a'));
SELECT 'expression key, NULL in its INCLUDE column' AS lookup, * FROM slow_compares(case_query_col('lk_incx', 'coalesce(s, ''{}''::int[]) = ''{}'' AND t = ''t'' || o.n::int', 'LIMIT 1', 'v'));
DROP FUNCTION slow_compares(text);
DROP TABLE lk_incn, lk_nullk, lk_incx;

DO $$ DECLARE t record; BEGIN FOR t IN SELECT ord FROM types LOOP EXECUTE format('DROP TABLE lk_%s', t.ord); END LOOP; END $$;
DROP FUNCTION order_function_calls(text), types_query(types), case_query_ud(text, text), step_io(text, text, text, text, text), case_query_col(text, text, text, text), step(text, text, text, text), case_query(text, text, text), probe_check(text), check_query(text);
DROP TABLE types, ud, entries, lk_dup, lk_run, lk_back, lk_io, lk_inc;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
