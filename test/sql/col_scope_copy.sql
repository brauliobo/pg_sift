-- sift.scope_create: a copy of a fact table keyed by a scope key the facts reach through a scope relation of unique (member, scope key) pairs. The copy holds
-- one row per (fact, scope key of its foreign key), has a sift_col index keyed by the scope key and is kept exact by statement triggers in the transaction of the change.
-- Ids belong to 0 to 6 scope keys; every edit below leaves the copy equal to the join (sift.scope_verify) and every key answers like the join.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int NOT NULL, created_at timestamptz NOT NULL, kind smallint NOT NULL, amount numeric(8,2) NOT NULL, note text)
	WITH (autovacuum_enabled = false);
INSERT INTO facts SELECT i, 1 + i % 60, timestamptz '2026-01-01' + i * interval '3 hours', i % 5, i % 97, 'n' FROM generate_series(1, 30000) i;
CREATE TABLE pairs (dim_a_id int NOT NULL, scope_id int NOT NULL, PRIMARY KEY (scope_id, dim_a_id)) WITH (autovacuum_enabled = false);
-- member e has 0 (e % 10 = 0), 1, 2, 3, 4 or 6 keys: the keys of a member are 1..n
INSERT INTO pairs SELECT e, k FROM generate_series(1, 60) e, generate_series(1, CASE e % 10 WHEN 0 THEN 0 WHEN 6 THEN 2 WHEN 7 THEN 3 WHEN 8 THEN 4 WHEN 9 THEN 6 ELSE 1 END) k;

-- what a scope relation, a table and a key must be
CREATE TABLE no_pk (dim_a_id int NOT NULL, created_at timestamptz);
SELECT sift.scope_create('no_pk_copy', 'no_pk', 'dim_a_id', 'pairs', 'scope_id');
CREATE TABLE dup_pairs (dim_a_id int NOT NULL, scope_id int NOT NULL);
SELECT sift.scope_create('dup_copy', 'facts', 'dim_a_id', 'dup_pairs', 'scope_id');
CREATE TABLE text_pairs (dim_a_id int NOT NULL, scope_id text NOT NULL, PRIMARY KEY (scope_id, dim_a_id));
SELECT sift.scope_create('text_copy', 'facts', 'dim_a_id', 'text_pairs', 'scope_id');
SELECT sift.scope_create('facts', 'facts', 'dim_a_id', 'pairs', 'scope_id');
SELECT sift.scope_create('bad_columns', 'facts', 'dim_a_id', 'pairs', 'scope_id', columns => ARRAY['amount', 'nope']);
SELECT sift.scope_create('bad_order', 'facts', 'dim_a_id', 'pairs', 'scope_id', order_column => 'note');
-- a copy holds a row once per key of its member: a member in too many keys is refused with the measured figure
CREATE TABLE wide_pairs (dim_a_id int NOT NULL, scope_id int NOT NULL, PRIMARY KEY (scope_id, dim_a_id));
INSERT INTO wide_pairs SELECT e, k FROM generate_series(1, 60) e, generate_series(1, 7) k;
SELECT sift.scope_create('wide_copy', 'facts', 'dim_a_id', 'wide_pairs', 'scope_id');
DROP TABLE no_pk, dup_pairs, text_pairs, wide_pairs;

SELECT sift.scope_create('facts_by_scope', 'facts', 'dim_a_id', 'pairs', 'scope_id') AS copy_rows;
SELECT count(*) AS join_rows FROM facts f JOIN pairs p USING (dim_a_id);
SELECT indexdef FROM pg_indexes WHERE tablename = 'facts_by_scope' ORDER BY indexname;

-- the contract of a read: one key at a time; a fact is once in each of its keys
CREATE FUNCTION answers(copy regclass, k int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE c text; j text;
BEGIN
	EXECUTE format('SELECT md5(string_agg(t::text, ''|'' ORDER BY t::text)) FROM (SELECT kind, count(*), sum(amount) FROM %s WHERE scope_id = %s GROUP BY kind) t', copy, k) INTO c;
	EXECUTE format('SELECT md5(string_agg(t::text, ''|'' ORDER BY t::text)) FROM (SELECT kind, count(*), sum(amount) FROM facts WHERE dim_a_id IN (SELECT dim_a_id FROM pairs WHERE scope_id = %s) GROUP BY kind) t', k) INTO j;
	RETURN CASE WHEN c IS NOT DISTINCT FROM j THEN 'same' ELSE 'DIFFERENT' END;
END $$;
CREATE FUNCTION state(label text) RETURNS TABLE (edit text, copy_rows bigint, differing bigint, keys_differing bigint) LANGUAGE sql AS $$
	SELECT label, (SELECT count(*) FROM facts_by_scope), (SELECT count(*) FROM sift.scope_verify('facts_by_scope')),
		   (SELECT count(*) FROM generate_series(1, 8) k WHERE answers('facts_by_scope', k) <> 'same') $$;

CREATE TABLE edits (n serial, label text, stmt text);
INSERT INTO edits (label, stmt) VALUES
	('insert facts of ids with 1, 3 and 6 keys', 'INSERT INTO facts SELECT 100000 + i, (ARRAY[1, 7, 9])[1 + i % 3] + 10 * (i % 4), now(), i % 5, i, ''x'' FROM generate_series(1, 30) i'),
	('insert facts of ids with no key', 'INSERT INTO facts SELECT 100100 + i, 10 * (1 + i % 5), now(), 1, i, ''x'' FROM generate_series(1, 20) i'),
	('update an amount', 'UPDATE facts SET amount = amount + 1 WHERE id % 7 = 0'),
	('update a column the copy does not read', 'UPDATE facts SET note = ''y'' WHERE id % 5 = 0'),
	('move facts to a member with other keys', 'UPDATE facts SET dim_a_id = 1 + (dim_a_id + 3) % 60 WHERE id % 11 = 0'),
	('change a primary key', 'UPDATE facts SET id = id + 200000 WHERE id % 13 = 0'),
	('delete facts of a member in 6 keys (every replica goes)', 'DELETE FROM facts WHERE dim_a_id = 9'),
	('delete facts of ids with no key', 'DELETE FROM facts WHERE dim_a_id % 10 = 0 AND id % 2 = 0'),
	('upsert facts', 'INSERT INTO facts SELECT i, 1 + i % 60, now(), 2, 1, ''u'' FROM generate_series(29990, 30010) i ON CONFLICT (id) DO UPDATE SET amount = facts.amount + 100'),
	('a member gains a key', 'INSERT INTO pairs VALUES (1, 5), (2, 5), (3, 5)'),
	('a member loses a key', 'DELETE FROM pairs WHERE dim_a_id IN (19, 29) AND scope_id IN (3, 4)'),
	('ids move between keys', 'UPDATE pairs SET scope_id = scope_id + 10 WHERE dim_a_id IN (16, 26, 36)'),
	('ids move back', 'UPDATE pairs SET scope_id = scope_id - 10 WHERE dim_a_id IN (16, 26, 36) AND scope_id > 10'),
	('a member with no fact gains keys', 'INSERT INTO pairs VALUES (99, 1), (99, 2)'),
	('every pair of a key goes', 'DELETE FROM pairs WHERE scope_id = 4'),
	('a key returns', 'INSERT INTO pairs SELECT dim_a_id, 4 FROM pairs WHERE scope_id = 3 ON CONFLICT DO NOTHING'),
	('the pair update changes nothing', 'UPDATE pairs SET dim_a_id = dim_a_id'),
	('pairs and facts change in one transaction', 'WITH d AS (DELETE FROM pairs WHERE scope_id = 2 RETURNING 1) INSERT INTO facts SELECT 100200 + i, 1 + i % 60, now(), 3, i, ''w'' FROM generate_series(1, 10) i'),
	('a pair and a fact of its member in one statement', 'WITH p AS (INSERT INTO pairs VALUES (50, 7) RETURNING 1) INSERT INTO facts SELECT 100300 + i, 50, now(), 4, i, ''v'' FROM generate_series(1, 5) i'),
	('truncate the pairs', 'TRUNCATE pairs'),
	('the pairs return', 'INSERT INTO pairs SELECT e, k FROM generate_series(1, 60) e, generate_series(1, 1 + e % 4) k'),
	('truncate the facts', 'TRUNCATE facts');
CREATE FUNCTION run_edits() RETURNS TABLE (edit text, copy_rows bigint, differing bigint, keys_differing bigint) LANGUAGE plpgsql AS $$
DECLARE e record;
BEGIN
	RETURN QUERY SELECT * FROM state('start');
	FOR e IN SELECT * FROM edits ORDER BY n LOOP
		EXECUTE e.stmt;
		RETURN QUERY SELECT * FROM state(e.label);
	END LOOP;
END $$;
-- TRUNCATE rebuilds the index and says what it chose each time
SET client_min_messages = warning;
SELECT * FROM run_edits();
RESET client_min_messages;

-- several keys in one read: a fact is once in each key, so IN counts it once per key; the member path or a DISTINCT on the primary key counts it once
INSERT INTO facts SELECT 100400 + i, 1 + i % 60, now(), i % 5, i, 'n' FROM generate_series(1, 400) i;
SELECT (SELECT count(*) FROM facts_by_scope WHERE scope_id IN (1, 2)) AS copy_in,
	   (SELECT count(DISTINCT id) FROM facts_by_scope WHERE scope_id IN (1, 2)) AS copy_distinct,
	   (SELECT count(*) FROM facts WHERE dim_a_id IN (SELECT dim_a_id FROM pairs WHERE scope_id IN (1, 2))) AS member_path,
	   (SELECT count(*) FROM facts f JOIN pairs p USING (dim_a_id) WHERE p.scope_id IN (1, 2)) AS join_rows;
-- GROUP BY the key counts a fact in each of its keys
SELECT scope_id, count(*) FROM facts_by_scope GROUP BY scope_id ORDER BY scope_id;

-- the copy is read by its sift index, with the answers of the normal plan
CREATE EXTENSION pg_sift_helpers;
VACUUM (FREEZE, ANALYZE) facts_by_scope;
SELECT sift_helpers.differences_agg('SELECT kind, count(*), sum(amount) FROM facts_by_scope WHERE scope_id = 3 GROUP BY kind') AS differences;
EXPLAIN (COSTS OFF) SELECT kind, count(*), sum(amount) FROM facts_by_scope WHERE scope_id = 3 GROUP BY kind;

-- the copy goes with its triggers
SELECT sift.scope_drop('facts_by_scope');
SELECT count(*) AS leftover_triggers FROM pg_trigger WHERE NOT tgisinternal AND tgrelid IN ('facts'::regclass, 'pairs'::regclass);
SELECT count(*) AS copies FROM sift.scope_copies;
SELECT sift.scope_drop('facts_by_scope');
DROP FUNCTION run_edits(), state(text), answers(regclass, int);
DROP TABLE edits, facts, pairs;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
