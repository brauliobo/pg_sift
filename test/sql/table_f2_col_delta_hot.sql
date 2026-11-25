-- Format 2.0 sift_table hot rows: a row inserted a moment ago and updated at once (in the same transaction, after a savepoint, in the next statement) keeps its ctid and no index takes an entry for
-- the update; a ROLLBACK TO SAVEPOINT gives the old values back. The entries of the indexes are counted on their pages. Every check raises on failure.
CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
CREATE EXTENSION pg_sift_helpers;
CREATE EXTENSION IF NOT EXISTS pageinspect;
SET sift.test_create_format1 = off;
SET check_function_bodies = off;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE SCHEMA f2cdh;
SET search_path = f2cdh, public;

-- the entries (live and dead) on the leaf pages of a B-tree index; the high key of a page that has a right sibling is not an entry
CREATE FUNCTION index_entries(idx regclass) RETURNS bigint LANGUAGE sql AS $$
	SELECT coalesce(sum(s.live_items + s.dead_items - (s.btpo_next <> 0)::int), 0) FROM generate_series(1, pg_relation_size(idx) / 8192 - 1) n, bt_page_stats(idx::text, n::int) s WHERE s.type = 'l' $$;

CREATE TABLE a (id int PRIMARY KEY, ref int, bal int, note text) USING sift_table;
CREATE TABLE ah (LIKE a INCLUDING ALL) USING heap;
CREATE INDEX a_ref ON a (ref);
INSERT INTO a SELECT i, i * 3, 0, 'x' FROM generate_series(1, 2000) i;
INSERT INTO ah SELECT * FROM a;
CREATE FUNCTION same_rows() RETURNS bool LANGUAGE sql AS $$
	SELECT NOT EXISTS (SELECT * FROM a EXCEPT SELECT * FROM ah) AND NOT EXISTS (SELECT * FROM ah EXCEPT SELECT * FROM a) $$;

-- 1. insert and update in one transaction
SELECT index_entries('a_ref') AS e0, index_entries('a_pkey') AS p0 \gset
BEGIN;
INSERT INTO a VALUES (5001, 15003, 1, 'hot');
SELECT ctid::text AS c1 FROM a WHERE id = 5001 \gset
UPDATE a SET bal = 2 WHERE id = 5001;
SELECT sift_helpers.assert_eq('the hot row keeps its ctid', ctid::text, :'c1') FROM a WHERE id = 5001;
COMMIT;
INSERT INTO ah VALUES (5001, 15003, 2, 'hot');
SELECT sift_helpers.assert_eq('the hot row has the new value', bal, 2) FROM a WHERE id = 5001;
SELECT sift_helpers.assert_eq('the ref index took the insert and no entry for the update', (index_entries('a_ref') - :e0)::int, 1);
SELECT sift_helpers.assert_eq('the primary key index took the insert and no entry for the update', (index_entries('a_pkey') - :p0)::int, 1);

-- 2. insert, savepoint, update, rollback to the savepoint, update again
SELECT index_entries('a_ref') AS e1 \gset
BEGIN;
INSERT INTO a VALUES (5002, 15006, 1, 'hot');
SAVEPOINT s;
UPDATE a SET bal = 9, note = 'undone' WHERE id = 5002;
SELECT sift_helpers.assert_eq('the savepoint sees the update', bal, 9) FROM a WHERE id = 5002;
ROLLBACK TO s;
SELECT sift_helpers.assert_eq('the rollback gives the old balance back', bal, 1) FROM a WHERE id = 5002;
SELECT sift_helpers.assert_eq('the rollback gives the old note back', note, 'hot') FROM a WHERE id = 5002;
UPDATE a SET bal = 3 WHERE id = 5002;
COMMIT;
INSERT INTO ah VALUES (5002, 15006, 3, 'hot');
SELECT sift_helpers.assert_eq('the final value', (bal, note)::text, '(3,hot)') FROM a WHERE id = 5002;
SELECT sift_helpers.assert_eq('one entry for the insert, none for the updates', (index_entries('a_ref') - :e1)::int, 1);

-- 3. a thousand rows inserted and updated by statements of their own
SELECT index_entries('a_ref') AS e2 \gset
CREATE TABLE hot_addr (id int, c text);
DO $$
BEGIN
	FOR i IN 1..300 LOOP
		EXECUTE format('INSERT INTO a VALUES (%s, %s, 1, ''h'')', 6000 + i, 18000 + i);
		EXECUTE format('INSERT INTO ah VALUES (%s, %s, 1, ''h'')', 6000 + i, 18000 + i);
		COMMIT;
	END LOOP;
END $$;
INSERT INTO hot_addr SELECT id, ctid::text FROM a WHERE id > 6000;
DO $$
BEGIN
	FOR i IN 1..300 LOOP
		EXECUTE format('UPDATE a SET bal = bal + 1 WHERE id = %s', 6000 + i);
		EXECUTE format('UPDATE ah SET bal = bal + 1 WHERE id = %s', 6000 + i);
		COMMIT;
	END LOOP;
END $$;
SELECT sift_helpers.assert_eq('no hot row moved', (SELECT count(*)::int FROM a JOIN hot_addr USING (id) WHERE a.ctid::text <> hot_addr.c), 0);
SELECT sift_helpers.assert_eq('the ref index took the inserts only', (index_entries('a_ref') - :e2)::int, 300);
SELECT sift_helpers.assert_eq('rows agree', same_rows(), true);
SELECT sift_helpers.assert_eq('sift.check is clean', count(*)::int, 0) FROM sift.check('a'::regclass);

DROP SCHEMA f2cdh CASCADE;
DROP EXTENSION pageinspect;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
