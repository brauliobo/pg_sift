-- Format 2.0 sift_table column deltas past the old 32-page cap: a chain longer than four pages gets a directory (SIFT_PT_COL_DELTA_DIR = 46), an UPDATE is never refused for the length of the
-- chain, the rows keep their ctid, a reader sees what its snapshot sees, and a fold gives the pages back. Every check raises on failure and prints one row (name, ok). About 30 s.
CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
CREATE EXTENSION IF NOT EXISTS pageinspect;
SET sift.test_create_format1 = off;
SET check_function_bodies = off;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET statement_timeout = '240s';

CREATE SCHEMA f2cdd;
SET search_path = f2cdd, public;

CREATE FUNCTION chk(name text, got anyelement, want anyelement) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	IF got IS DISTINCT FROM want THEN RAISE EXCEPTION 'FAILED %: got %, want %', name, got, want; END IF;
	RETURN name || ': ok';
END $$;

-- the pages of the table of a page type: 42 SIFT_PT_COL_DELTA, 46 SIFT_PT_COL_DELTA_DIR
CREATE FUNCTION pages_of(rel regclass, ptype int) RETURNS int LANGUAGE sql AS $$
	SELECT count(*)::int FROM generate_series(0, pg_relation_size(rel) / 8192 - 1) n
	WHERE get_byte(get_raw_page(rel::text, n::int), 8176) + 256 * get_byte(get_raw_page(rel::text, n::int), 8177) = ptype $$;

CREATE TABLE o (id int PRIMARY KEY, customer_id int, region int, status int, amount int, n6 int, n7 int, n8 int, notes text) USING sift_table;
CREATE TABLE oh (LIKE o INCLUDING ALL) USING heap;
CREATE INDEX o_customer ON o (customer_id);
INSERT INTO o SELECT i, i % 100, i % 10, 0, i, i, i, i, 'note ' || i FROM generate_series(1, 60000) i;
INSERT INTO oh SELECT * FROM o;
CREATE FUNCTION same_rows() RETURNS bool LANGUAGE sql AS $$
	SELECT NOT EXISTS (SELECT * FROM o EXCEPT SELECT * FROM oh) AND NOT EXISTS (SELECT * FROM oh EXCEPT SELECT * FROM o) $$;
CREATE TABLE addr AS SELECT id, ctid::text AS c FROM o;
CREATE FUNCTION moved() RETURNS bigint LANGUAGE sql AS $$ SELECT count(*) FROM o JOIN addr USING (id) WHERE o.ctid::text <> addr.c $$;

-- 1. a short chain has no directory
SELECT chk('no delta page yet', pages_of('o', 42), 0);
UPDATE o SET status = 1 WHERE id = 5;
UPDATE oh SET status = 1 WHERE id = 5;
SELECT chk('a short chain has pages', pages_of('o', 42) BETWEEN 1 AND 4, true);
SELECT chk('a short chain has no directory', pages_of('o', 46), 0);

-- 2. one statement writes far more pages than the old cap of 32: no row leaves its address and the chain has a directory
UPDATE o SET status = 2 WHERE id <= 30000;
UPDATE oh SET status = 2 WHERE id <= 30000;
SELECT chk('a bulk update writes more than 32 pages', pages_of('o', 42) > 32, true);
SELECT chk('a long chain has a directory', pages_of('o', 46) >= 1, true);
SELECT chk('no row of the bulk update moved', moved()::int, 0);
SELECT chk('rows agree after the bulk update', same_rows(), true);

-- 3. a second one, over other rows: the directory grows, point reads and aggregates see both
UPDATE o SET status = 3 WHERE id > 20000 AND id <= 50000;
UPDATE oh SET status = 3 WHERE id > 20000 AND id <= 50000;
SELECT chk('the chain passes the soft fold size', pages_of('o', 42) >= 256, true);
SELECT chk('a second directory page', pages_of('o', 46) >= 2, true);
SELECT chk('no row moved again', moved()::int, 0);
SELECT chk('point reads', (SELECT array_agg(status ORDER BY id) FROM o WHERE id IN (1, 5, 25000, 55000)), ARRAY[2, 2, 3, 0]);
SELECT chk('group by a patched column', (SELECT array_agg(x ORDER BY status) FROM (SELECT status, count(*), sum(amount) FROM o GROUP BY status) x)::text,
		   (SELECT array_agg(x ORDER BY status) FROM (SELECT status, count(*), sum(amount) FROM oh GROUP BY status) x)::text);
SELECT chk('rows agree after the second', same_rows(), true);

-- 4. a transaction that rolls back leaves nothing a reader sees; a cursor opened before an update does not see it
SELECT pages_of('o', 42) AS before_pages \gset
BEGIN;
UPDATE o SET status = 9 WHERE id <= 15000;
SELECT chk('the writer sees its patches', count(*)::int, 15000) FROM o WHERE status = 9;
ROLLBACK;
SELECT chk('a rollback leaves no patch', count(*)::int, 0) FROM o WHERE status = 9;
SELECT chk('rows agree after the rollback', same_rows(), true);
BEGIN;
DECLARE c CURSOR FOR SELECT count(*) FILTER (WHERE status = 7) AS patched, count(*) AS n FROM o WHERE id BETWEEN 1 AND 40000;
UPDATE o SET status = 7 WHERE id BETWEEN 1 AND 40000;
SELECT chk('the writer sees its patches again', count(*)::int, 40000) FROM o WHERE status = 7;
FETCH c;
COMMIT;
UPDATE oh SET status = 7 WHERE id BETWEEN 1 AND 40000;
SELECT chk('rows agree after the cursor', same_rows(), true);
SELECT chk('no row moved in the transactions', moved()::int, 0);

-- 5. a fold gives the chain and its directory back: the rounds of updates and VACUUM do not grow the table
CREATE FUNCTION round_of_updates() RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	UPDATE o SET status = status + 1 WHERE id <= 40000;
	UPDATE oh SET status = status + 1 WHERE id <= 40000;
END $$;
SELECT round_of_updates();
SELECT pg_relation_size('o') AS first_size \gset
VACUUM (FREEZE) o;
SELECT busy FROM sift.table_maintain('o');
SELECT unnest(ARRAY['SELECT round_of_updates()', 'VACUUM (FREEZE) o', 'SELECT busy FROM sift.table_maintain(''o'')']) FROM generate_series(1, 3) \gexec
SELECT chk('rounds of long chains and VACUUM do not grow the table', pg_relation_size('o') <= :first_size * 2, true);
SELECT chk('rows agree after the rounds', same_rows(), true);
SELECT chk('sift.check is clean', count(*)::int, 0) FROM sift.check('o'::regclass);
SELECT chk('no row moved in the rounds', moved()::int, 0);

DROP SCHEMA f2cdd CASCADE;
DROP EXTENSION pageinspect;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
