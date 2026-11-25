-- Format 2.0 sift_table parity: every check raises on failure and prints one row (name, ok); a clean run prints only ok rows.
-- About 15 s. Run on a build where new tables take Format 2.0 (sift.test_create_format1 off) except in section 12, which makes a Format 1 table.
CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
CREATE EXTENSION pg_sift_helpers;
CREATE EXTENSION IF NOT EXISTS pageinspect;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET statement_timeout = '240s';

CREATE SCHEMA f2p;
SET search_path = f2p, public;

CREATE FUNCTION fails(stmt text, state text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE stmt;
	RAISE EXCEPTION 'FAILED: % did not raise %', stmt, state;
EXCEPTION WHEN OTHERS THEN
	IF SQLSTATE <> state THEN RAISE EXCEPTION 'FAILED: % raised % (%), want %', stmt, SQLSTATE, SQLERRM, state; END IF;
	RETURN state || ': ok';
END $$;

CREATE FUNCTION format_major(rel regclass) RETURNS int LANGUAGE sql AS $$
	SELECT (get_byte(page, 28) + 256 * get_byte(page, 29))::int FROM (SELECT get_raw_page(rel::text, 0) AS page) p $$;

-- 1. CRUD
CREATE TABLE t (id int PRIMARY KEY, g int, v numeric(10,2), s text, ts timestamptz) USING sift_table;
INSERT INTO t SELECT i, i % 10, i / 4.0, 'row ' || i, timestamptz '2024-01-01' + i * interval '1 minute' FROM generate_series(1, 20000) i;
SELECT sift_helpers.assert_eq('insert count', count(*)::int, 20000) FROM t;
SELECT sift_helpers.assert_eq('insert sum', sum(v), (SELECT sum(i / 4.0) FROM generate_series(1, 20000) i)::numeric) FROM t;
UPDATE t SET v = v + 1, s = s || '!' WHERE g = 3;
SELECT sift_helpers.assert_eq('update rows', count(*)::int, 2000) FROM t WHERE s LIKE '%!';
SELECT sift_helpers.assert_eq('update value', v, 13 / 4.0 + 1) FROM t WHERE id = 13;
DELETE FROM t WHERE g = 7;
SELECT sift_helpers.assert_eq('delete count', count(*)::int, 18000) FROM t;
SELECT sift_helpers.assert_eq('deleted gone', count(*)::int, 0) FROM t WHERE g = 7;
SELECT sift_helpers.assert_eq('pk probe', s, 'row 5') FROM t WHERE id = 5;
SELECT sift_helpers.assert_eq('pk probe of a deleted row', count(*)::int, 0) FROM t WHERE id = 7;
SELECT sift_helpers.assert_eq('format major', format_major('t'), 2);
SELECT sift_helpers.assert_eq('ctid is virtual', (SELECT count(*)::int FROM t WHERE ctid = (SELECT ctid FROM t WHERE id = 5)), 1);
SELECT sift_helpers.assert_eq('tid range', (SELECT count(*)::int FROM t WHERE ctid >= '(0,1)' AND ctid < '(1,1)') BETWEEN 1 AND 291, true);
SELECT sift_helpers.assert_eq('tablesample', (SELECT count(*) FROM t TABLESAMPLE SYSTEM (100)) = 18000, true);

-- 2. MVCC: rollback, repeatable read, read committed, subtransactions
BEGIN;
INSERT INTO t VALUES (100001, 1, 1, 'rolled back', now());
UPDATE t SET v = -1 WHERE id = 1;
DELETE FROM t WHERE id = 2;
ROLLBACK;
SELECT sift_helpers.assert_eq('rollback insert', count(*)::int, 0) FROM t WHERE id = 100001;
SELECT sift_helpers.assert_eq('rollback update', v <> -1, true) FROM t WHERE id = 1;
SELECT sift_helpers.assert_eq('rollback delete', count(*)::int, 1) FROM t WHERE id = 2;
BEGIN;
SAVEPOINT a;
DELETE FROM t WHERE id = 3;
ROLLBACK TO a;
SELECT sift_helpers.assert_eq('rollback to savepoint', count(*)::int, 1) FROM t WHERE id = 3;
SAVEPOINT b;
INSERT INTO t VALUES (100002, 1, 1, 'kept', now());
RELEASE b;
COMMIT;
SELECT sift_helpers.assert_eq('release savepoint', count(*)::int, 1) FROM t WHERE id = 100002;
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT count(*) FROM t \gset before_
INSERT INTO t VALUES (100003, 1, 1, 'inside', now());
SELECT sift_helpers.assert_eq('own insert visible', count(*)::int, :before_count + 1) FROM t;
ROLLBACK;
SELECT sift_helpers.assert_eq('own insert rolled back', count(*)::int, :before_count) FROM t;
BEGIN;
DECLARE c CURSOR FOR SELECT id FROM t WHERE id <= 3 ORDER BY id;
UPDATE t SET v = 0 WHERE id <= 3;
SELECT sift_helpers.assert_eq('cursor snapshot', (SELECT count(*)::int FROM t WHERE id <= 3 AND v = 0), 3);
FETCH ALL FROM c;
ROLLBACK;

-- 3. COPY, COPY FREEZE, multi_insert, CTAS
CREATE TABLE c (id int, s text, n numeric(12,3)) USING sift_table;
COPY (SELECT i, 'c' || i, i / 8.0 FROM generate_series(1, 30000) i) TO '/tmp/f2p_copy.csv' CSV;
COPY c FROM '/tmp/f2p_copy.csv' CSV;
SELECT sift_helpers.assert_eq('copy count', count(*)::int, 30000) FROM c;
SELECT sift_helpers.assert_eq('copy sum', sum(n), (SELECT sum(i / 8.0) FROM generate_series(1, 30000) i)::numeric) FROM c;
TRUNCATE c;
BEGIN;
TRUNCATE c;
COPY c FROM '/tmp/f2p_copy.csv' CSV FREEZE;
COMMIT;
SELECT sift_helpers.assert_eq('copy freeze count', count(*)::int, 30000) FROM c;
CREATE TABLE ctas USING sift_table AS SELECT * FROM c WHERE id % 2 = 0;
SELECT sift_helpers.assert_eq('ctas count', count(*)::int, 15000) FROM ctas;
SELECT sift_helpers.assert_eq('ctas format', format_major('ctas'), 2);
INSERT INTO c SELECT * FROM ctas;
SELECT sift_helpers.assert_eq('insert select', count(*)::int, 45000) FROM c;
\! rm -f /tmp/f2p_copy.csv

-- 4. ON CONFLICT
CREATE TABLE oc (id int PRIMARY KEY, n int, s text) USING sift_table;
INSERT INTO oc SELECT i, 1, 'a' FROM generate_series(1, 1000) i;
INSERT INTO oc SELECT i, 1, 'b' FROM generate_series(1, 1500) i ON CONFLICT (id) DO NOTHING;
SELECT sift_helpers.assert_eq('do nothing', (count(*)::int, count(*) FILTER (WHERE s = 'a')::int), (1500, 1000)) FROM oc;
INSERT INTO oc SELECT i, 1, 'c' FROM generate_series(1, 1200) i ON CONFLICT (id) DO UPDATE SET n = oc.n + 1, s = EXCLUDED.s;
SELECT sift_helpers.assert_eq('do update', (count(*)::int, count(*) FILTER (WHERE n = 2)::int, count(*) FILTER (WHERE s = 'c')::int), (1500, 1200, 1200)) FROM oc;
SELECT fails('INSERT INTO oc VALUES (1, 0, ''dup'')', '23505');
MERGE INTO oc USING (SELECT i AS id FROM generate_series(1400, 1600) i) m ON oc.id = m.id
	WHEN MATCHED THEN UPDATE SET n = 9 WHEN NOT MATCHED THEN INSERT VALUES (m.id, 9, 'm');
SELECT sift_helpers.assert_eq('merge', (count(*)::int, count(*) FILTER (WHERE n = 9)::int), (1600, 201)) FROM oc;

-- 5. Row locks: the four modes, NOWAIT, SKIP LOCKED, EvalPlanQual through a second session
SELECT sift_helpers.assert_eq('for update', count(*)::int, 5) FROM (SELECT id FROM oc WHERE id <= 5 FOR UPDATE) l;
SELECT sift_helpers.assert_eq('for no key update', count(*)::int, 5) FROM (SELECT id FROM oc WHERE id <= 5 FOR NO KEY UPDATE) l;
SELECT sift_helpers.assert_eq('for share', count(*)::int, 5) FROM (SELECT id FROM oc WHERE id <= 5 FOR SHARE) l;
SELECT sift_helpers.assert_eq('for key share', count(*)::int, 5) FROM (SELECT id FROM oc WHERE id <= 5 FOR KEY SHARE) l;
CREATE EXTENSION IF NOT EXISTS dblink;
SELECT dblink_connect('s2', format('dbname=%s user=%s', current_database(), current_user));
BEGIN;
SELECT count(*) FROM (SELECT 1 FROM oc WHERE id = 10 FOR UPDATE) l;
SELECT fails($$SELECT * FROM dblink('s2', 'SELECT 1 FROM f2p.oc WHERE id = 10 FOR UPDATE NOWAIT') AS x(r int)$$, '55P03');
SELECT sift_helpers.assert_eq('skip locked', (SELECT count(*)::int FROM dblink('s2', 'SELECT id FROM f2p.oc WHERE id IN (10, 11) FOR UPDATE SKIP LOCKED') AS x(id int)), 1);
ROLLBACK;
-- EvalPlanQual: the second session waits on a row the first updates, then re-evaluates its qual on the successor
SELECT dblink_exec('s2', 'SET search_path = f2p');
BEGIN;
UPDATE oc SET n = 100 WHERE id = 20;
SELECT dblink_send_query('s2', 'UPDATE oc SET n = n + 1 WHERE id = 20 AND n >= 2');
DO $$
DECLARE deadline timestamptz := clock_timestamp() + interval '10 s';
BEGIN
	WHILE NOT EXISTS (SELECT 1 FROM pg_stat_activity WHERE wait_event_type = 'Lock' AND pid <> pg_backend_pid() AND datname = current_database()) LOOP
		IF clock_timestamp() > deadline THEN RAISE EXCEPTION 'the second session did not wait on the row'; END IF;
		PERFORM pg_sleep(0.01);
	END LOOP;
END $$;
COMMIT;
SELECT sift_helpers.assert_eq('epq update count', r, 'UPDATE 1') FROM dblink_get_result('s2') AS x(r text);
SELECT sift_helpers.assert_eq('epq successor', n, 101) FROM oc WHERE id = 20;
SELECT dblink_disconnect('s2');

-- 6. Secondary B-tree, partial, expression, unique and covering indexes; Index Only Scan reports no heap fetches that skip MVCC
CREATE INDEX t_g ON t (g);
CREATE INDEX t_expr ON t (lower(s));
CREATE INDEX t_part ON t (id) WHERE g = 3;
CREATE UNIQUE INDEX t_uniq ON t (id, g);
SET enable_seqscan = off;
SELECT sift_helpers.assert_eq('btree probe', count(*)::int, 2000) FROM t WHERE g = 3;
SELECT sift_helpers.assert_eq('expression probe', count(*)::int, 1) FROM t WHERE lower(s) = 'row 5';
SELECT sift_helpers.assert_eq('partial probe', count(*)::int, 2000) FROM t WHERE g = 3 AND id > 0;
SET enable_bitmapscan = off;
SELECT sift_helpers.assert_eq('index scan after update', (SELECT count(*)::int FROM t WHERE g = 3 AND s LIKE '%!'), 2000);
RESET enable_bitmapscan;
RESET enable_seqscan;
SET enable_indexscan = off; SET enable_seqscan = off;
SELECT sift_helpers.assert_eq('bitmap scan', count(*)::int, 2000) FROM t WHERE g = 3;
RESET enable_indexscan; RESET enable_seqscan;
SELECT sift_helpers.assert_eq('index vs seq', (SELECT count(*) FROM t WHERE g BETWEEN 2 AND 4), count(*)) FROM (SELECT * FROM t WHERE g BETWEEN 2 AND 4 OFFSET 0) x;
UPDATE t SET g = 11 WHERE id <= 100;
SELECT sift_helpers.assert_eq('index follows update', count(*)::int, 90) FROM t WHERE g = 11;
CREATE INDEX CONCURRENTLY t_cic ON t (ts);
SELECT sift_helpers.assert_eq('cic valid', indisvalid, true) FROM pg_index WHERE indexrelid = 't_cic'::regclass;
SELECT sift_helpers.assert_eq('cic probe', count(*)::int, 1) FROM t WHERE ts = timestamptz '2024-01-01' + 5 * interval '1 minute';
REINDEX TABLE CONCURRENTLY t;
SELECT sift_helpers.assert_eq('reindex valid', count(*) FILTER (WHERE NOT indisvalid)::int, 0) FROM pg_index WHERE indrelid = 't'::regclass;

-- 7. BM25 index on a table: visibility of the rows it scores
CREATE TABLE d (id int PRIMARY KEY, body text) USING sift_table;
INSERT INTO d SELECT i, 'alpha beta ' || (CASE WHEN i % 3 = 0 THEN 'gamma' ELSE 'delta' END) || ' doc' || i FROM generate_series(1, 3000) i;
CREATE INDEX d_bm25 ON d USING sift (body);
SELECT sift_helpers.assert_eq('bm25 match', count(*)::int, 1000) FROM d WHERE body @@> 'gamma';
DELETE FROM d WHERE id % 6 = 0;
SELECT sift_helpers.assert_eq('bm25 after delete', count(*)::int, 500) FROM d WHERE body @@> 'gamma';
BEGIN;
UPDATE d SET body = 'omega only' WHERE id = 3;
SELECT sift_helpers.assert_eq('bm25 own update', count(*)::int, 1) FROM d WHERE body @@> 'omega';
ROLLBACK;
SELECT sift_helpers.assert_eq('bm25 after rollback', count(*)::int, 0) FROM d WHERE body @@> 'omega';

-- 8. Wide values: stored outside the granules, read back, updated, compacted
CREATE TABLE w (id int PRIMARY KEY, big text, b bytea, j jsonb) USING sift_table;
INSERT INTO w SELECT i, repeat(md5(i::text), 400), decode(repeat(md5(i::text), 300), 'hex') , jsonb_build_object('k', repeat('x', 6000), 'i', i)
	FROM generate_series(1, 200) i;
SELECT sift_helpers.assert_eq('wide length', (min(length(big)), max(length(big))), (12800, 12800)) FROM w;
SELECT sift_helpers.assert_eq('wide content', big, repeat(md5('7'), 400)) FROM w WHERE id = 7;
SELECT sift_helpers.assert_eq('wide bytea', length(b), 4800) FROM w WHERE id = 7;
SELECT sift_helpers.assert_eq('wide jsonb', j ->> 'i', '7') FROM w WHERE id = 7;
UPDATE w SET big = repeat('z', 20000) WHERE id <= 50;
SELECT sift_helpers.assert_eq('wide update', count(*)::int, 50) FROM w WHERE big = repeat('z', 20000);
DELETE FROM w WHERE id > 100;
VACUUM w;
SELECT sift_helpers.assert_eq('wide after vacuum', (count(*)::int, count(*) FILTER (WHERE length(big) >= 12800)::int), (100, 100)) FROM w;
SELECT sift_helpers.assert_eq('wide aggregate', sum(length(big))::bigint, 50::bigint * 20000 + 50::bigint * 12800) FROM w;

-- 9. ALTER TABLE: ADD, RENAME and DROP COLUMN keep the relation file; ALTER COLUMN TYPE rewrites it into a new one
CREATE TABLE a (id int, v int) USING sift_table;
INSERT INTO a SELECT i, i FROM generate_series(1, 5000) i;
SELECT pg_relation_filenode('a') AS fn0 \gset
ALTER TABLE a ADD COLUMN extra text DEFAULT 'x';
ALTER TABLE a ADD COLUMN nothing int;
SELECT sift_helpers.assert_eq('add column keeps the file', pg_relation_filenode('a') = :fn0, true);
SELECT sift_helpers.assert_eq('add column default', (count(*) FILTER (WHERE extra = 'x')::int, count(nothing)::int), (5000, 0)) FROM a;
INSERT INTO a VALUES (5001, 1, 'y', 7);
ALTER TABLE a ALTER COLUMN v TYPE bigint;
SELECT sift_helpers.assert_eq('widen rewrites', pg_relation_filenode('a') <> :fn0, true);
SELECT pg_relation_filenode('a') AS fn1 \gset
SELECT sift_helpers.assert_eq('widen values', sum(v), (SELECT sum(i) + 1 FROM generate_series(1, 5000) i)::numeric) FROM a;
ALTER TABLE a RENAME COLUMN v TO val;
SELECT sift_helpers.assert_eq('rename column', sum(val), (SELECT sum(i) + 1 FROM generate_series(1, 5000) i)::numeric) FROM a;
ALTER TABLE a DROP COLUMN nothing;
SELECT sift_helpers.assert_eq('drop column', (SELECT count(*)::int FROM pg_attribute WHERE attrelid = 'a'::regclass AND attnum > 0 AND NOT attisdropped), 3);
SELECT sift_helpers.assert_eq('drop column keeps the file', pg_relation_filenode('a') = :fn1, true);
ALTER TABLE a ALTER COLUMN val TYPE text USING val::text;
SELECT sift_helpers.assert_eq('recast values', (SELECT min(val) FROM a WHERE id = 5000), '5000');
ALTER TABLE a ADD COLUMN gen int GENERATED ALWAYS AS (id * 2) STORED;
SELECT sift_helpers.assert_eq('stored generated column', gen, 20) FROM a WHERE id = 10;
-- a column named by an option cannot be dropped; renaming it renames the option value
CREATE TABLE ao (id int, ts timestamp, v int) USING sift_table;
SELECT sift.table_set_option('ao', 'key_order', 'ts');
SELECT fails('ALTER TABLE ao DROP COLUMN ts', '2BP01');
ALTER TABLE ao RENAME COLUMN ts TO tm;
SELECT sift_helpers.assert_eq('option follows rename', value, 'tm') FROM sift.table_option_values WHERE rel::oid = 'ao'::regclass AND name = 'key_order';
SELECT sift.table_reset_option('ao', 'key_order');
ALTER TABLE ao DROP COLUMN tm;
SELECT sift_helpers.assert_eq('drop after reset', (SELECT count(*)::int FROM pg_attribute WHERE attrelid = 'ao'::regclass AND attnum > 0 AND NOT attisdropped), 2);

-- 10. VACUUM FREEZE advances relfrozenxid; index cleanup; VACUUM (TRUNCATE) keeps the rows
CREATE TABLE vf (id int PRIMARY KEY, v int) USING sift_table;
INSERT INTO vf SELECT i, i FROM generate_series(1, 8000) i;
SELECT age(relfrozenxid) AS age0 FROM pg_class WHERE oid = 'vf'::regclass \gset
DELETE FROM vf WHERE id % 2 = 0;
VACUUM (FREEZE, INDEX_CLEANUP ON, VERBOSE OFF) vf;
SELECT sift_helpers.assert_eq('freeze relfrozenxid advanced', age(relfrozenxid) <= :age0 + 1, true) FROM pg_class WHERE oid = 'vf'::regclass;
SELECT sift_helpers.assert_eq('freeze rows', (count(*)::int, sum(v)::bigint), (4000, (SELECT sum(i) FROM generate_series(1, 7999, 2) i))) FROM vf;
SELECT sift_helpers.assert_eq('vacuum drops the index entries of dead rows', (SELECT count(*)::int FROM vf WHERE id = 2), 0);
SET enable_seqscan = off;
SELECT sift_helpers.assert_eq('index after vacuum', (SELECT count(*)::int FROM vf WHERE id BETWEEN 1 AND 100), 50);
RESET enable_seqscan;
DELETE FROM vf WHERE id > 100;
VACUUM (TRUNCATE ON) vf;
SELECT sift_helpers.assert_eq('rows after tail', count(*)::int, 50) FROM vf;
SELECT sift_helpers.assert_eq('stats rows', reltuples::int BETWEEN 0 AND 100, true) FROM pg_class WHERE oid = 'vf'::regclass;
SELECT sift_helpers.assert_eq('allvisible is not claimed', relallvisible, 0) FROM pg_class WHERE oid = 'vf'::regclass;
ANALYZE vf;
SELECT sift_helpers.assert_eq('analyze', (SELECT reltuples::int FROM pg_class WHERE oid = 'vf'::regclass) BETWEEN 1 AND 200, true);
VACUUM FULL vf;
SELECT sift_helpers.assert_eq('vacuum full', (count(*)::int, format_major('vf')), (50, 2)) FROM vf;
CLUSTER vf USING vf_pkey;
SELECT sift_helpers.assert_eq('cluster', count(*)::int, 50) FROM vf;

-- 11. TRUNCATE: plain, in a transaction, rolled back, with a staged write, restart identity, of a published table
CREATE TABLE tr (id serial PRIMARY KEY, v int) USING sift_table;
INSERT INTO tr (v) SELECT i FROM generate_series(1, 10000) i;
BEGIN; TRUNCATE tr; SELECT sift_helpers.assert_eq('truncate inside', count(*)::int, 0) FROM tr; ROLLBACK;
SELECT sift_helpers.assert_eq('truncate rolled back', count(*)::int, 10000) FROM tr;
BEGIN; INSERT INTO tr (v) VALUES (1); TRUNCATE tr; COMMIT;
SELECT sift_helpers.assert_eq('truncate after staged write', count(*)::int, 0) FROM tr;
INSERT INTO tr (v) SELECT i FROM generate_series(1, 10) i;
TRUNCATE tr RESTART IDENTITY;
INSERT INTO tr (v) VALUES (1);
SELECT sift_helpers.assert_eq('restart identity', id, 1) FROM tr;
SELECT sift_helpers.assert_eq('truncate format', format_major('tr'), 2);
SELECT sift_helpers.assert_eq('virtual addresses restart', ctid::text, '(0,1)') FROM tr;
CREATE TABLE trp (id int) USING sift_table;
INSERT INTO trp SELECT generate_series(1, 100);
CREATE TABLE trf (id int REFERENCES tr (id)) USING sift_table;
SELECT fails('TRUNCATE tr', '0A000');
TRUNCATE tr, trf;

-- 12. FOR ALL TABLES publication (needs the library preloaded; the refusal without it is in C-INTEGRATION-GUIDE.md)
SET client_min_messages = error;
CREATE PUBLICATION f2p_all FOR ALL TABLES;
SET client_min_messages = warning;
CREATE TABLE pub (id int PRIMARY KEY, v text) USING sift_table;
INSERT INTO pub SELECT i, 'p' || i FROM generate_series(1, 1000) i;
UPDATE pub SET v = 'u' WHERE id <= 10;
DELETE FROM pub WHERE id > 990;
SELECT sift_helpers.assert_eq('published dml', (count(*)::int, count(*) FILTER (WHERE v = 'u')::int), (990, 10)) FROM pub;
ALTER TABLE pub REPLICA IDENTITY FULL;
UPDATE pub SET v = 'f' WHERE id = 11;
SELECT sift_helpers.assert_eq('replica identity full', v, 'f') FROM pub WHERE id = 11;
SELECT sift_helpers.assert_eq('replica identity changed', relreplident, 'f') FROM pg_class WHERE oid = 'pub'::regclass;
CREATE TABLE pub_part (id int, v text) PARTITION BY RANGE (id);
CREATE TABLE pub_p1 (id int, v text) USING sift_table;
ALTER TABLE pub_part ATTACH PARTITION pub_p1 FOR VALUES FROM (0) TO (100);
SELECT sift_helpers.assert_eq('attach partition', count(*)::int, 0) FROM pub_part;
DROP TABLE pub_part;
DROP TABLE pub;
DROP PUBLICATION f2p_all;

-- 13. sift.upgrade_table(regclass) RETURNS bool: a Format 1 table becomes Format 2.0 with its rows, indexes and options; false (and a notice) when there is nothing to do; MAINTAIN is the right
SET sift.test_create_format1 = on;
CREATE TABLE up (id int PRIMARY KEY, g int, s text, ts timestamptz) USING sift_table;
INSERT INTO up SELECT i, i % 7, repeat('u', i % 50), timestamptz '2024-01-01' + i * interval '1 second' FROM generate_series(1, 40000) i;
SET sift.test_create_format1 = off;
UPDATE up SET s = 'upd' WHERE id <= 100;
DELETE FROM up WHERE id > 39000;
CREATE INDEX up_g ON up (g);
SELECT sift.table_set_option('up', 'key_order', 'ts');
SELECT sift_helpers.assert_eq('before upgrade', format_major('up'), 1);
SELECT count(*), sum(id), md5(string_agg(s, ',' ORDER BY id)) FROM up \gset up0_
SELECT pg_relation_filenode('up') AS fn_up \gset
SELECT sift_helpers.assert_eq('upgrade result', sift.upgrade_table('up'), true);
SELECT sift_helpers.assert_eq('after upgrade', format_major('up'), 2);
SELECT sift_helpers.assert_eq('upgrade swapped the file', pg_relation_filenode('up') <> :fn_up, true);
SELECT sift_helpers.assert_eq('upgrade count', count(*)::int, :up0_count::int) FROM up;
SELECT sift_helpers.assert_eq('upgrade sum', sum(id)::numeric, :up0_sum::numeric) FROM up;
SELECT sift_helpers.assert_eq('upgrade content', md5(string_agg(s, ',' ORDER BY id)), :'up0_md5') FROM up;
SELECT sift_helpers.assert_eq('upgrade keeps the indexes', (SELECT count(*)::int FROM pg_index WHERE indrelid = 'up'::regclass AND indisvalid), 2);
SELECT sift_helpers.assert_eq('upgrade keeps the options', value, 'ts') FROM sift.table_option_values WHERE rel::oid = 'up'::regclass AND name = 'key_order';
SET enable_seqscan = off;
SELECT sift_helpers.assert_eq('index after upgrade', count(*)::int, (SELECT count(*)::int FROM generate_series(1, 39000) i WHERE i % 7 = 3)) FROM up WHERE g = 3;
RESET enable_seqscan;
SELECT pg_relation_filenode('up') AS fn_up2 \gset
SELECT sift_helpers.assert_eq('upgrade twice', sift.upgrade_table('up'), false);
SELECT sift_helpers.assert_eq('upgrade twice keeps the file', pg_relation_filenode('up') = :fn_up2, true);
INSERT INTO up VALUES (50000, 1, 'after', now());
SELECT sift_helpers.assert_eq('write after upgrade', count(*)::int, 39001) FROM up;
VACUUM FREEZE up;
CREATE ROLE f2p_user NOLOGIN;
GRANT USAGE ON SCHEMA f2p TO f2p_user;
SET ROLE f2p_user;
SELECT fails('SELECT sift.upgrade_table(''f2p.up'')', '42501');
RESET ROLE;
GRANT MAINTAIN ON up TO f2p_user;
SET ROLE f2p_user;
SELECT sift_helpers.assert_eq('maintain may upgrade', sift.upgrade_table('f2p.up'), false);
RESET ROLE;

-- 14. No visibility map fraction for the planner (EXT-06)
ANALYZE c;
SELECT sift_helpers.assert_eq('relallvisible', relallvisible, 0) FROM pg_class WHERE oid = 'c'::regclass;

-- 15. The options: ten documented, thirteen of Format 1 that are accepted and ignored, the grammar of compression of sift_col
CREATE TABLE opt (id int, ts timestamptz, s text) USING sift_table;
SELECT sift_helpers.assert_eq('documented options', (SELECT count(*)::int FROM unnest(sift._table_option_names())), 10);
SELECT sift.table_set_option('opt', 'compression', 'auto; s=zstd:3');
SELECT sift_helpers.assert_eq('compression per column', value, 'auto; s=zstd:3') FROM sift.table_option_values WHERE rel::oid = 'opt'::regclass AND name = 'compression';
SELECT fails('SELECT sift.table_set_option(''opt'', ''compression'', ''s=bogus'')', '22023');
SELECT fails('SELECT sift.table_set_option(''opt'', ''no_such_option'', ''x'')', '22023');
SELECT sift.table_set_option('opt', 'wide_rows', 'on');
SELECT sift_helpers.assert_eq('format 1 option kept', value, 'on') FROM sift.table_option_values WHERE rel::oid = 'opt'::regclass AND name = 'wide_rows';
SELECT sift.table_reset_option('opt', 'wide_rows');
SELECT sift_helpers.assert_eq('format 1 option reset', count(*)::int, 0) FROM sift.table_option_values WHERE rel::oid = 'opt'::regclass AND name = 'wide_rows';
INSERT INTO opt SELECT i, now(), repeat('o', 10) FROM generate_series(1, 1000) i;
SELECT sift_helpers.assert_eq('table with options', count(*)::int, 1000) FROM opt;

-- 16. Virtual generated columns (PostgreSQL 18) hold no value in the table (EXT-12)
SELECT current_setting('server_version_num')::int >= 180000 AS pg18 \gset
\if :pg18
CREATE TABLE gv (id int PRIMARY KEY, a int, v int GENERATED ALWAYS AS (a * 2) VIRTUAL, s text, ts timestamptz) USING sift_table;
INSERT INTO gv (id, a, s, ts) SELECT i, i, 'r' || i, now() FROM generate_series(1, 5000) i;
SELECT sift_helpers.assert_eq('virtual column evaluates', (sum(v), count(*) FILTER (WHERE v = a * 2)::int), (25005000::bigint, 5000)) FROM gv;
UPDATE gv SET a = a + 1 WHERE id <= 100;
SELECT sift_helpers.assert_eq('virtual column follows update', v, 22) FROM gv WHERE id = 10;
ALTER TABLE gv ADD COLUMN w int GENERATED ALWAYS AS (a + 100) VIRTUAL;
ALTER TABLE gv ADD COLUMN k int DEFAULT 5;
SELECT sift_helpers.assert_eq('virtual column added later', (count(*) FILTER (WHERE w = a + 100)::int, count(*) FILTER (WHERE k = 5)::int), (5000, 5000)) FROM gv;
ALTER TABLE gv DROP COLUMN v;
VACUUM gv;
SELECT sift_helpers.assert_eq('after drop and vacuum', (count(*)::int, sum(w)::bigint), (5000, 13002600::bigint)) FROM gv;
\endif

-- 17. A lookup by address finds its row among many parts: one part for each batch of a hundred rows
CREATE TABLE lp (id int PRIMARY KEY, v int) USING sift_table;
DO $$ BEGIN FOR i IN 0..39 LOOP INSERT INTO lp SELECT i * 100 + n, n FROM generate_series(1, 100) n; COMMIT; END LOOP; END $$;
SELECT sift_helpers.assert_eq('probe every batch', (SELECT count(*)::int FROM generate_series(0, 39) i, LATERAL (SELECT 1 FROM lp WHERE id = i * 100 + 50) x), 40);
UPDATE lp SET v = -v WHERE id % 100 = 50;
SELECT sift_helpers.assert_eq('update every batch', count(*)::int, 40) FROM lp WHERE v = -50;
SELECT sift_helpers.assert_eq('read an empty stage', count(*)::int, 4000) FROM lp;

DROP SCHEMA f2p CASCADE;
DROP OWNED BY f2p_user;
DROP ROLE f2p_user;
DROP EXTENSION pageinspect;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
