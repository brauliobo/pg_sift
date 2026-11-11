-- sift.scope_create: a copy of a fact table keyed by a scope key the facts reach through a scope relation of unique (member, scope key) pairs. The copy holds
-- one row per (fact, scope key of its foreign key), has a sift_col index keyed by the scope key and is kept exact by statement triggers in the transaction of the change.
-- Ids belong to 0 to 6 scope keys; every edit below leaves the copy equal to the join (sift.scope_verify) and every key answers like the join.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int NOT NULL, created_at timestamptz NOT NULL, kind smallint NOT NULL, amount numeric(8,2) NOT NULL, note text)
	WITH (autovacuum_enabled = false);
CREATE INDEX ON facts (dim_a_id);
INSERT INTO facts SELECT i, CASE WHEN i % 2 = 0 THEN 1 + (i / 2) % 60 ELSE 10 * ((i / 2) % 6) + 1 + (i / 12) % 5 END, timestamptz '2026-01-01' + i * interval '3 hours', i % 5, i % 97, 'n' FROM generate_series(1, 30000) i;
CREATE TABLE pairs (dim_a_id int NOT NULL, scope_id int NOT NULL, PRIMARY KEY (scope_id, dim_a_id)) WITH (autovacuum_enabled = false);
-- member e has 0 (e % 10 = 0), 1, 2, 3, 4 or 6 keys: the keys of a member are 1..n
INSERT INTO pairs SELECT e, k FROM generate_series(1, 60) e, generate_series(1, CASE e % 10 WHEN 0 THEN 0 WHEN 6 THEN 2 WHEN 7 THEN 3 WHEN 8 THEN 4 WHEN 9 THEN 6 ELSE 1 END) k;

-- what a scope relation, a table and a key must be
CREATE TABLE no_pk (dim_a_id int NOT NULL, created_at timestamptz);
SELECT sift.scope_create('no_pk_copy', 'no_pk', 'dim_a_id', 'pairs', 'scope_id');
CREATE TABLE dup_pairs (dim_a_id int NOT NULL, scope_id int NOT NULL);
SELECT sift.scope_create('dup_copy', 'facts', 'dim_a_id', 'dup_pairs', 'scope_id');
-- a pair that comes in finds the rows of its member by an index that starts with the foreign key: a table without one, with the key not first or with a partial index is refused
CREATE TABLE no_fk_index (id int PRIMARY KEY, dim_a_id int NOT NULL);
SELECT sift.scope_create('no_index_copy', 'no_fk_index', 'dim_a_id', 'pairs', 'scope_id');
CREATE INDEX ON no_fk_index (id, dim_a_id);
SELECT sift.scope_create('no_index_copy', 'no_fk_index', 'dim_a_id', 'pairs', 'scope_id');
CREATE INDEX ON no_fk_index (dim_a_id) WHERE id > 0;
SELECT sift.scope_create('no_index_copy', 'no_fk_index', 'dim_a_id', 'pairs', 'scope_id');
CREATE TABLE text_pairs (dim_a_id int NOT NULL, scope_id text NOT NULL, PRIMARY KEY (scope_id, dim_a_id));
SELECT sift.scope_create('text_copy', 'facts', 'dim_a_id', 'text_pairs', 'scope_id');
SELECT sift.scope_create('facts', 'facts', 'dim_a_id', 'pairs', 'scope_id');
SELECT sift.scope_create('bad_columns', 'facts', 'dim_a_id', 'pairs', 'scope_id', columns => ARRAY['amount', 'nope']);
SELECT sift.scope_create('bad_order', 'facts', 'dim_a_id', 'pairs', 'scope_id', columns => ARRAY['amount'], order_column => 'note');
-- a copy holds a row once per key of its member: a member in too many keys is refused with the measured figure
CREATE TABLE wide_pairs (dim_a_id int NOT NULL, scope_id int NOT NULL, PRIMARY KEY (scope_id, dim_a_id));
INSERT INTO wide_pairs SELECT e, k FROM generate_series(1, 60) e, generate_series(1, 7) k;
SELECT sift.scope_create('wide_copy', 'facts', 'dim_a_id', 'wide_pairs', 'scope_id');
-- the refusal is a default: max_copies raises it, and the NOTICE gives the same figures
CREATE TABLE triple_pairs (dim_a_id int NOT NULL, scope_id int NOT NULL, PRIMARY KEY (scope_id, dim_a_id));
INSERT INTO triple_pairs SELECT e, k FROM generate_series(1, 60) e, generate_series(1, 3) k;
SELECT sift.scope_create('triple_copy', 'facts', 'dim_a_id', 'triple_pairs', 'scope_id');
SELECT sift.scope_create('triple_copy', 'facts', 'dim_a_id', 'triple_pairs', 'scope_id', max_copies => 3.1) AS copy_rows;
SELECT sift.scope_drop('triple_copy');
SELECT sift.scope_create('triple_copy', 'facts', 'dim_a_id', 'triple_pairs', 'scope_id', max_copies => 0.9);
SELECT sift.scope_create('triple_copy', 'facts', 'dim_a_id', 'triple_pairs', 'scope_id', max_copies => NULL);
DROP TABLE triple_pairs;
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
-- replicas_off: the copy's rows minus the copies the histogram of sift.scope_replicas counts; multi_differing: problems of a check over a set of keys
CREATE FUNCTION state(label text) RETURNS TABLE (edit text, copy_rows bigint, differing bigint, keys_differing bigint, replicas_off bigint, multi_differing bigint) LANGUAGE sql AS $$
	SELECT label, (SELECT count(*) FROM facts_by_scope), (SELECT count(*) FROM sift.scope_verify('facts_by_scope')),
		   (SELECT count(*) FROM generate_series(1, 8) k WHERE answers('facts_by_scope', k) <> 'same'),
		   (SELECT count(*) FROM facts_by_scope) - COALESCE((SELECT sum(copies_per_fact * facts) FROM sift.scope_replicas('facts_by_scope')), 0),
		   (SELECT count(*) FROM sift.scope_verify('facts_by_scope', ARRAY[1, 2, 3, 5, 7])) $$;

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
CREATE FUNCTION run_edits() RETURNS TABLE (edit text, copy_rows bigint, differing bigint, keys_differing bigint, replicas_off bigint, multi_differing bigint) LANGUAGE plpgsql AS $$
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

-- the exact read over several keys: a fact counts under the first requested key that holds its member (sift.scope_read_keys); same answers as the member path, any order, 2 or 3 keys
CREATE FUNCTION read_answers(ks bigint[]) RETURNS text LANGUAGE plpgsql AS $$
DECLARE c text; j text;
BEGIN
	EXECUTE format('SELECT md5(string_agg(t::text, ''|'' ORDER BY t::text)) FROM (SELECT kind, count(*), sum(amount) FROM facts_by_scope WHERE %s GROUP BY kind) t',
				   sift.scope_read_keys('facts_by_scope', ks)) INTO c;
	EXECUTE 'SELECT md5(string_agg(t::text, ''|'' ORDER BY t::text)) FROM (SELECT kind, count(*), sum(amount) FROM facts WHERE dim_a_id IN (SELECT dim_a_id FROM pairs WHERE scope_id = ANY ($1)) GROUP BY kind) t'
		INTO j USING ks;
	RETURN CASE WHEN c IS NOT DISTINCT FROM j THEN 'same' ELSE 'DIFFERENT' END;
END $$;
CREATE FUNCTION read_rows(ks bigint[]) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	EXECUTE format('SELECT count(*) FROM facts_by_scope WHERE %s', sift.scope_read_keys('facts_by_scope', ks)) INTO n;
	RETURN n;
END $$;
CREATE TABLE read_sets (n serial, keys bigint[]);
INSERT INTO read_sets (keys) VALUES (ARRAY[1]), (ARRAY[1, 2]), (ARRAY[2, 1]), (ARRAY[3, 4]), (ARRAY[1, 2, 3]), (ARRAY[3, 2, 1]), (ARRAY[2, 4, 1]), (ARRAY[4, 9, 3]), (ARRAY[2, 2, 1]), (ARRAY[9]),
	(ARRAY[]::bigint[]);
SELECT r.keys, read_answers(r.keys) AS answers,
	   (SELECT count(*) FROM facts_by_scope WHERE scope_id = ANY (r.keys)) AS copy_in,
	   read_rows(r.keys) AS exact_rows,
	   (SELECT count(*) FROM facts WHERE dim_a_id IN (SELECT dim_a_id FROM pairs WHERE scope_id = ANY (r.keys))) AS member_path
FROM read_sets r ORDER BY r.n;
SELECT sift.scope_read_keys('facts_by_scope', ARRAY[3, 1, 3, 2]) AS condition;
SELECT sift.scope_read_keys('facts_by_scope', ARRAY[1, 2], 'f') AS aliased;
SELECT sift.scope_read_keys('facts', ARRAY[1]);

-- how many copies the facts have: the facts by the number of keys of their member (0: no copy)
SELECT * FROM sift.scope_replicas('facts_by_scope');
SELECT sift.scope_replicas('facts');

-- sift.scope_verify over a set of keys checks those keys and the facts a read over the set sees once each
CREATE TABLE key_sets (n serial, label text, keys bigint[]);
INSERT INTO key_sets (label, keys) VALUES ('one key', ARRAY[1]), ('two keys', ARRAY[1, 2]), ('keys 1 to 4', ARRAY[1, 2, 3, 4]), ('a key no pair has', ARRAY[9]),
	('keys with and without pairs', ARRAY[3, 9]), ('no key', ARRAY[]::bigint[]);
SELECT k.label, (SELECT count(*) FROM sift.scope_verify('facts_by_scope', k.keys)) AS problems FROM key_sets k ORDER BY k.n;
-- a copy damaged with the triggers off: each case names its problems for the keys it checks (the damage is undone after each)
CREATE FUNCTION damaged(stmt text, keys bigint[]) RETURNS TABLE (problem text, n bigint) LANGUAGE plpgsql AS $$
DECLARE found jsonb;
BEGIN
	BEGIN
		SET LOCAL session_replication_role = replica;
		EXECUTE stmt;
		SELECT COALESCE(jsonb_object_agg(v.problem, v.n), '{}') INTO found FROM (SELECT p.problem, count(*) n FROM sift.scope_verify('facts_by_scope', keys) p GROUP BY 1) v;
		RAISE EXCEPTION '%', found;
	EXCEPTION WHEN raise_exception THEN
		found := SQLERRM::jsonb;
	END;
	RETURN QUERY SELECT e.key, e.value::bigint FROM jsonb_each_text(found) e ORDER BY 1;
END $$;
-- the fact 100401 belongs to the member with the keys 1, 2 and 3
CREATE TABLE damage (n serial, label text, stmt text, keys bigint[]);
INSERT INTO damage (label, stmt, keys) VALUES
	('a fact lost from every key', 'DELETE FROM facts_by_scope WHERE id = 100401', ARRAY[1, 2, 3]),
	('a replica lost in the first requested key', 'DELETE FROM facts_by_scope WHERE id = 100401 AND scope_id = 1', ARRAY[1, 2]),
	('a replica lost in the second requested key', 'DELETE FROM facts_by_scope WHERE id = 100401 AND scope_id = 1', ARRAY[2, 1]),
	('a replica lost, the set is that key', 'DELETE FROM facts_by_scope WHERE id = 100401 AND scope_id = 1', ARRAY[1]),
	('a replica lost in a key outside the set', 'DELETE FROM facts_by_scope WHERE id = 100401 AND scope_id = 1', ARRAY[2, 3]),
	('a copy in a key the member does not have', 'INSERT INTO facts_by_scope SELECT 4, id, dim_a_id, created_at, kind, amount, note FROM facts WHERE id = 100401', ARRAY[4]),
	('the same, a key of the member is requested first', 'INSERT INTO facts_by_scope SELECT 4, id, dim_a_id, created_at, kind, amount, note FROM facts WHERE id = 100401', ARRAY[1, 4]),
	('the same, the key of the copy is requested first', 'INSERT INTO facts_by_scope SELECT 4, id, dim_a_id, created_at, kind, amount, note FROM facts WHERE id = 100401', ARRAY[4, 1]),
	('a fact twice in a key', 'INSERT INTO facts_by_scope SELECT * FROM facts_by_scope WHERE id = 100401 AND scope_id = 1', ARRAY[1]),
	('a stale amount', 'UPDATE facts_by_scope SET amount = amount + 1 WHERE id = 100401 AND scope_id = 2', ARRAY[2]),
	('a stale amount in a key outside the set', 'UPDATE facts_by_scope SET amount = amount + 1 WHERE id = 100401 AND scope_id = 2', ARRAY[1]),
	('a fact lost, every key checked', 'DELETE FROM facts_by_scope WHERE id = 100401', NULL);
SELECT d.label, COALESCE(v.problem, '(none)') AS problem, v.n FROM damage d LEFT JOIN LATERAL damaged(d.stmt, d.keys) v ON true ORDER BY d.n, v.problem;
SELECT count(*) AS problems_after FROM sift.scope_verify('facts_by_scope', ARRAY[1, 2, 3, 4]);

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
DROP FUNCTION run_edits(), state(text), answers(regclass, int), damaged(text, bigint[]), read_answers(bigint[]), read_rows(bigint[]);
DROP TABLE edits, key_sets, damage, read_sets, facts, pairs;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
