-- CH-02 / Spec 41 lane 2: bulk loads of a sift_table (the direct route of COPY and CREATE TABLE AS, the staged route of a small COPY, a trigger and a single INSERT) and ALTER TABLE ADD / DROP COLUMN
-- that rewrites no part. Every check raises on failure and prints one row (name, ok); the same statements run on a heap table (the oracle) and every row and column must agree. Run on a build where
-- new tables take Format 2.0. Needs a superuser (COPY to a server file).
SET client_min_messages = warning;
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
\set am sift_table
SET max_parallel_workers_per_gather = 0;
SET statement_timeout = '240s';

CREATE SCHEMA f2c;
SET search_path = f2c, public;

CREATE FUNCTION chk(name text, got anyelement, want anyelement) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	IF got IS DISTINCT FROM want THEN RAISE EXCEPTION 'FAILED %: got %, want %', name, got, want; END IF;
	RETURN name || ': ok';
END $$;

CREATE FUNCTION fails(stmt text, state text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE stmt;
	RAISE EXCEPTION 'FAILED: % did not raise %', stmt, state;
EXCEPTION WHEN OTHERS THEN
	IF SQLSTATE <> state THEN RAISE EXCEPTION 'FAILED: % raised % (%), want %', stmt, SQLSTATE, SQLERRM, state; END IF;
	RETURN state || ': ok';
END $$;

-- every row and column of two tables of the same shape
CREATE FUNCTION same(name text, a regclass, b regclass) RETURNS text LANGUAGE plpgsql AS $$
DECLARE x text; y text;
BEGIN
	EXECUTE format('SELECT md5(string_agg(t::text, ''|'' ORDER BY t.id)) FROM %s t', a) INTO x;
	EXECUTE format('SELECT md5(string_agg(t::text, ''|'' ORDER BY t.id)) FROM %s t', b) INTO y;
	IF x IS DISTINCT FROM y THEN RAISE EXCEPTION 'FAILED %: % differs from %', name, a, b; END IF;
	RETURN name || ': ok';
END $$;

-- a statement on the table under test and on its oracle (%1$I is the name)
CREATE FUNCTION on_both(tpl text, tbl text, oracle text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format(tpl, tbl);
	EXECUTE format(tpl, oracle);
END $$;

-- a query on both, as sets of rows
CREATE FUNCTION agg_same(name text, tpl text, tbl text, oracle text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE d int;
BEGIN
	EXECUTE format('SELECT count(*) FROM ((%s EXCEPT %s) UNION ALL (%s EXCEPT %s)) x', format(tpl, tbl), format(tpl, oracle), format(tpl, oracle), format(tpl, tbl)) INTO d;
	IF d <> 0 THEN RAISE EXCEPTION 'FAILED %: % rows differ', name, d; END IF;
	RETURN name || ': ok';
END $$;

CREATE FUNCTION file_of(n text) RETURNS text LANGUAGE sql AS $$ SELECT '/tmp/sift_copy_schema_evol_' || n || '.tsv' $$;

CREATE VIEW src AS
SELECT i AS id, i % 10 AS g, (i / 4.0)::numeric(10,2) AS v, 'row ' || i AS s, date '2020-01-01' + i % 700 AS d, i * 0.5::float8 AS f, CASE WHEN i % 11 = 0 THEN NULL ELSE i % 1000 END AS k
FROM generate_series(1, 30000) i;

-- 1. the direct route: COPY and CREATE TABLE AS of 30000 rows (batches of 1000 slots), against a heap oracle
CREATE TABLE tb (id int, g int, v numeric(10,2), s text, d date, f float8, k int) USING :am;
CREATE TABLE ob (LIKE tb) USING heap;
COPY (SELECT * FROM src) TO '/tmp/sift_copy_schema_evol_main.tsv';
COPY tb FROM '/tmp/sift_copy_schema_evol_main.tsv';
COPY ob FROM '/tmp/sift_copy_schema_evol_main.tsv';
SELECT chk('copy count', count(*)::int, 30000) FROM tb;
SELECT chk('copy sum', sum(v), (SELECT sum(v) FROM src)) FROM tb;
SELECT same('copy rows', 'tb', 'ob');
CREATE TABLE tc USING :am AS SELECT * FROM src;
CREATE TABLE oc AS SELECT * FROM src;
SELECT chk('ctas count', count(*)::int, 30000) FROM tc;
SELECT same('ctas rows', 'tc', 'oc');
SELECT chk('address of a loaded row', count(*)::int, 1) FROM tb WHERE ctid = (SELECT ctid FROM tb WHERE id = 12345);
SELECT chk('probe of a loaded row', s, 'row 12345') FROM tb WHERE id = 12345;

-- 2. the staged route: a small COPY, a single INSERT, INSERT ... SELECT, a trigger
CREATE TABLE ts (LIKE tb) USING :am;
CREATE TABLE os (LIKE tb) USING heap;
COPY (SELECT * FROM src WHERE id <= 10) TO '/tmp/sift_copy_schema_evol_small.tsv';
COPY ts FROM '/tmp/sift_copy_schema_evol_small.tsv';
COPY os FROM '/tmp/sift_copy_schema_evol_small.tsv';
INSERT INTO ts SELECT * FROM src WHERE id BETWEEN 11 AND 40;
INSERT INTO os SELECT * FROM src WHERE id BETWEEN 11 AND 40;
SELECT on_both('INSERT INTO %1$I SELECT * FROM src WHERE id = 41', 'ts', 'os');
SELECT chk('small copy and inserts', count(*)::int, 41) FROM ts;
SELECT same('staged rows', 'ts', 'os');
CREATE TABLE klog (n int);
CREATE FUNCTION klog_row() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN INSERT INTO klog VALUES (1); RETURN NULL; END $$;
CREATE TABLE tk (LIKE tb) USING :am;
CREATE TRIGGER tk_after AFTER INSERT ON tk FOR EACH ROW EXECUTE FUNCTION klog_row();
COPY tk FROM '/tmp/sift_copy_schema_evol_main.tsv';
SELECT chk('row trigger fired for every row', (SELECT count(*)::int FROM klog), 30000);
SELECT same('rows with a trigger', 'tk', 'ob');

-- 3. rollback, subtransactions and a read of the transaction's own rows
CREATE TABLE tr (LIKE tb) USING :am;
BEGIN;
COPY tr FROM '/tmp/sift_copy_schema_evol_main.tsv';
SELECT chk('own bulk rows are read', count(*)::int, 30000) FROM tr;
SELECT chk('own bulk rows by key', s, 'row 777') FROM tr WHERE id = 777;
ROLLBACK;
SELECT chk('rolled back copy', count(*)::int, 0) FROM tr;
BEGIN;
SAVEPOINT a;
COPY tr FROM '/tmp/sift_copy_schema_evol_main.tsv';
ROLLBACK TO a;
SELECT chk('rolled back savepoint', count(*)::int, 0) FROM tr;
COPY tr FROM '/tmp/sift_copy_schema_evol_main.tsv';
SAVEPOINT b;
DELETE FROM tr WHERE g = 3;
RELEASE b;
COMMIT;
SELECT chk('committed copy after a savepoint', count(*)::int, 27000) FROM tr;
SELECT chk('deleted by the same transaction', count(*)::int, 0) FROM tr WHERE g = 3;
CREATE TABLE tcheck (LIKE tb, CHECK (id < 29990)) USING :am;
SELECT fails('COPY tcheck FROM ''/tmp/sift_copy_schema_evol_main.tsv''', '23514');
SELECT chk('a failed copy leaves nothing', count(*)::int, 0) FROM tcheck;
INSERT INTO tr SELECT * FROM src WHERE g = 3;
SELECT chk('a later insert after the rollbacks', count(*)::int, 30000) FROM tr;

-- 4. ALTER TABLE ADD COLUMN with a default rewrites nothing: old rows read the constant, new rows keep their values
CREATE TABLE sz1 AS SELECT c.relfilenode AS node, pg_relation_size(c.oid) AS bytes FROM pg_class c WHERE c.oid = 'tb'::regclass;
SELECT on_both('ALTER TABLE %1$I ADD COLUMN c1 int DEFAULT 7', 'tb', 'ob');
SELECT on_both('ALTER TABLE %1$I ADD COLUMN c2 text DEFAULT ''abc''', 'tb', 'ob');
SELECT on_both('ALTER TABLE %1$I ADD COLUMN c3 float8 DEFAULT 2.5', 'tb', 'ob');
SELECT on_both('ALTER TABLE %1$I ADD COLUMN c4 int', 'tb', 'ob');
SELECT on_both('ALTER TABLE %1$I ADD COLUMN c5 date DEFAULT ''2020-02-29''', 'tb', 'ob');
SELECT on_both('ALTER TABLE %1$I ADD COLUMN c6 numeric DEFAULT 1.25', 'tb', 'ob');
SELECT on_both('ALTER TABLE %1$I ADD COLUMN c7 bool DEFAULT true', 'tb', 'ob');
SELECT on_both('ALTER TABLE %1$I ADD COLUMN c8 uuid DEFAULT ''00000000-0000-0000-0000-00000000002a''', 'tb', 'ob');
SELECT on_both('ALTER TABLE %1$I ADD COLUMN c9 text DEFAULT repeat(''x'', 3000)', 'tb', 'ob');
SELECT chk('add column rewrote no file', c.relfilenode = b.node, true) FROM pg_class c, sz1 b WHERE c.oid = 'tb'::regclass;
SELECT chk('add column wrote no byte', pg_relation_size('tb') = b.bytes, true) FROM sz1 b;
SELECT chk('old rows read an int default', count(*) FILTER (WHERE c1 = 7)::int, 30000) FROM tb;
SELECT chk('old rows read a text default', count(*) FILTER (WHERE c2 = 'abc')::int, 30000) FROM tb;
SELECT chk('old rows read a float default', sum(c3), 75000::float8) FROM tb;
SELECT chk('old rows read no value for a plain column', count(c4)::int, 0) FROM tb;
SELECT chk('old rows read a date default', count(*) FILTER (WHERE c5 = date '2020-02-29')::int, 30000) FROM tb;
SELECT chk('old rows read a numeric default', sum(c6), 37500.00) FROM tb;
SELECT chk('old rows read a bool default', bool_and(c7), true) FROM tb;
SELECT chk('old rows read a uuid default', count(DISTINCT c8)::int, 1) FROM tb;
SELECT chk('old rows read a long text default', min(length(c9)), 3000) FROM tb;
SELECT same('after add column', 'tb', 'ob');
SELECT agg_same('aggregate over added columns', 'SELECT g, sum(c1), max(c3), count(c4), min(c5), sum(c6), bool_and(c7), min(c2), min(length(c9)) FROM %s GROUP BY g', 'tb', 'ob');
SELECT agg_same('filter on an added column', 'SELECT id, c1 FROM %s WHERE c1 = 7 AND c2 = ''abc'' AND id %% 997 = 0', 'tb', 'ob');

-- new rows after the columns were added are written with their values; old and new rows are read together
COPY (SELECT id + 30000, g, v, s, d, f, k, 9, 'new ' || id, 1.5::float8, id, date '2021-01-01', 2.75::numeric, false, '00000000-0000-0000-0000-000000000001'::uuid, 'y' FROM src) TO '/tmp/sift_copy_schema_evol_more.tsv';
COPY tb FROM '/tmp/sift_copy_schema_evol_more.tsv';
COPY ob FROM '/tmp/sift_copy_schema_evol_more.tsv';
SELECT chk('rows after the columns', count(*)::int, 60000) FROM tb;
SELECT chk('new rows keep their values', count(*) FILTER (WHERE c1 = 9 AND c2 LIKE 'new %' AND NOT c7)::int, 30000) FROM tb;
SELECT same('old and new rows', 'tb', 'ob');
SELECT agg_same('aggregate over old and new rows', 'SELECT c1, c2 LIKE ''new %%'' AS fresh, count(*), sum(c6), max(c5), count(c4), min(c8::text) FROM %s GROUP BY 1, 2', 'tb', 'ob');
SELECT on_both('UPDATE %1$I SET c1 = 100 WHERE id %% 25 = 0', 'tb', 'ob');
SELECT on_both('DELETE FROM %1$I WHERE id %% 13 = 0', 'tb', 'ob');
SELECT same('update and delete over old rows', 'tb', 'ob');
SELECT chk('an updated old row keeps its other defaults', c2, 'abc') FROM tb WHERE id = 5;

-- 5. DROP COLUMN rewrites nothing and the column is gone; a column added after it is a new attribute
CREATE TABLE sz2 AS SELECT c.relfilenode AS node, pg_relation_size(c.oid) AS bytes FROM pg_class c WHERE c.oid = 'tb'::regclass;
SELECT on_both('ALTER TABLE %1$I DROP COLUMN v', 'tb', 'ob');
SELECT on_both('ALTER TABLE %1$I DROP COLUMN c2', 'tb', 'ob');
SELECT chk('drop column rewrote no file', c.relfilenode = b.node, true) FROM pg_class c, sz2 b WHERE c.oid = 'tb'::regclass;
SELECT chk('drop column wrote no byte', pg_relation_size('tb') = b.bytes, true) FROM sz2 b;
SELECT fails('SELECT v FROM tb', '42703');
SELECT chk('dropped columns are out of the row', (SELECT count(*)::int FROM pg_attribute WHERE attrelid = 'tb'::regclass AND attnum > 0 AND NOT attisdropped), 14);
SELECT same('after drop column', 'tb', 'ob');
SELECT on_both('ALTER TABLE %1$I ADD COLUMN c10 int DEFAULT 1', 'tb', 'ob');
SELECT on_both('ALTER TABLE %1$I ALTER COLUMN c1 SET DEFAULT 99', 'tb', 'ob');
SELECT on_both('INSERT INTO %1$I (id, g) VALUES (100001, 1)', 'tb', 'ob');
SELECT chk('a new default applies to new rows only', (SELECT c1 FROM tb WHERE id = 100001), 99);
SELECT chk('an old row keeps the default it was added with', (SELECT c1 FROM tb WHERE id = 1), 7);
SELECT chk('a column added after a drop reads its default', (SELECT count(*) FILTER (WHERE c10 = 1)::int FROM tb), (SELECT count(*)::int FROM tb));
SELECT same('after a column added past a dropped one', 'tb', 'ob');

-- 6. the rewrite of old parts under the current schema (compaction, VACUUM, VACUUM FULL) changes no value
VACUUM tb;
VACUUM ob;
SELECT same('after vacuum', 'tb', 'ob');
SELECT agg_same('aggregate after vacuum', 'SELECT g, sum(c1), max(c3), count(c4), min(c5), sum(c6), bool_and(c7), min(length(c9)), sum(c10) FROM %s GROUP BY g', 'tb', 'ob');
VACUUM (FREEZE) tb;
SELECT same('after vacuum freeze', 'tb', 'ob');
VACUUM FULL tb;
VACUUM FULL ob;
SELECT same('after vacuum full', 'tb', 'ob');
SELECT chk('the oracle count', (SELECT count(*)::int FROM ob), (SELECT count(*)::int FROM tb));

\! rm -f /tmp/sift_copy_schema_evol_main.tsv /tmp/sift_copy_schema_evol_small.tsv /tmp/sift_copy_schema_evol_more.tsv
DROP SCHEMA f2c CASCADE;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
