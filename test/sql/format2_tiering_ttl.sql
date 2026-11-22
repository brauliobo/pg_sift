-- Object storage tiering and TTL of a sift_table: the policy functions, their checks and their privileges, the move of a part to a directory store and its read back, sift.check of the object (sound,
-- damaged, gone), that the policies follow a rename and go with a drop, the TTL drop of a part that is in the store, a B-tree that still holds the entries of the dropped rows, and that tiering changes
-- no query result. The text column keeps every part larger than a page: a part that small holds its stream in its header and is never moved.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse
SELECT current_setting('data_directory') || '/sift_tier_regress' AS tierdir \gset
\set mk 'rm -rf ' :tierdir ' ' :tierdir '.bak && mkdir -p ' :tierdir
COPY (SELECT 1) TO PROGRAM :'mk';
CREATE TABLE ev (id int8, ts timestamp NOT NULL, tenant int, v int, pad text) USING sift_table;
CREATE TABLE evn (ts timestamp) USING sift_table;
CREATE TABLE plain (ts timestamp NOT NULL);
INSERT INTO ev SELECT i, timestamp '2020-01-01' + i * interval '1 hour', i % 7, i % 100, md5(i::text) FROM generate_series(1, 5000) i;
VACUUM ev;
INSERT INTO ev SELECT 5000 + i, localtimestamp - i * interval '1 minute', i % 7, i % 100, md5((5000 + i)::text) FROM generate_series(1, 5000) i;
VACUUM ev;
SELECT count(*) AS parts, min(level) AS min_level, max(level) AS max_level FROM sift.parts('ev');
SELECT count(*) AS rows, sum(v) AS sum_v FROM ev;
SELECT md5(string_agg(id || ':' || ts || ':' || v || pad, ',' ORDER BY id)) AS before FROM ev \gset

-- no policy yet
SELECT count(*) AS parts FROM sift.tier_status;
SELECT sift.clear_tier_policy('ev') AS cleared;
SELECT sift.set_ttl('ev', 'ts', NULL) AS cleared;

-- what is refused
SELECT sift.set_tier_policy('ev', 'ftp://example.com/p');
SELECT sift.set_tier_policy('ev', 'file://relative/dir');
SELECT sift.set_tier_policy('ev', 'file:///tmp/a?x=1');
SELECT sift.set_tier_policy('ev', 'file://' || :'tierdir', interval '0');
SELECT sift.set_tier_policy('plain', 'file://' || :'tierdir');
SELECT sift.set_ttl('ev', 'tenant', interval '30 days');
SELECT sift.set_ttl('ev', 'ts', interval '0');
SELECT sift.set_ttl('evn', 'ts', interval '30 days');
SELECT sift.set_ttl('plain', 'ts', interval '30 days');
CREATE ROLE tier_probe;
SET ROLE tier_probe;
SELECT sift.set_ttl('ev', 'ts', interval '30 days');
SELECT sift.set_tier_policy('ev', 'file://' || :'tierdir');
SELECT sift.clear_tier_policy('ev');
SELECT count(*) AS parts FROM sift.tier_status;
RESET ROLE;
DROP ROLE tier_probe;

-- a policy: set, set again, changed; the VACUUM moves the part whose newest row is older than move_after
SELECT sift.set_tier_policy('ev', 'file://' || :'tierdir', '30 days') AS changed;
SELECT sift.set_tier_policy('ev', 'file://' || :'tierdir', '30 days') AS changed;
SELECT sift.set_tier_policy('ev', 'file://' || :'tierdir', '45 days') AS changed;
SELECT relid::regclass AS relation, uri ~ '^file:///' AS local, move_after, endpoint IS NULL AS no_endpoint FROM sift.tier_policies;
VACUUM ev;
SELECT count(*) AS tiered, bool_and(location LIKE 'file:///%.siftpart') AS located, bool_and(byte_len > 0) AS sized, bool_and(expires_at IS NULL) AS staying FROM sift.tier_status;
SELECT md5(string_agg(id || ':' || ts || ':' || v || pad, ',' ORDER BY id)) = :'before' AS unchanged_by_tiering FROM ev;
SELECT count(*) AS old_rows, sum(v) AS old_sum_v FROM ev WHERE ts < localtimestamp - interval '45 days';
SELECT count(*) AS problems FROM sift.check('ev');

-- sift.check reads the object: a byte that changed and an object that is gone are damage of the table; the object put back is sound
SELECT substr(location, 8) AS objpath FROM sift.tier_status \gset
\set save 'cp ' :objpath ' ' :tierdir '.bak'
COPY (SELECT 1) TO PROGRAM :'save';
\set flip 'printf X | dd of=' :objpath ' bs=1 seek=100 conv=notrunc 2>/dev/null'
COPY (SELECT 1) TO PROGRAM :'flip';
SELECT count(*) AS problems, bool_and(detail LIKE '%object in the store%') AS names_the_object FROM sift.check('ev');
\set gone 'rm ' :objpath
COPY (SELECT 1) TO PROGRAM :'gone';
SELECT count(*) AS problems, bool_and(detail LIKE '%object in the store%') AS names_the_object FROM sift.check('ev');
\set put 'cp ' :tierdir '.bak ' :objpath
COPY (SELECT 1) TO PROGRAM :'put';
SELECT count(*) AS problems FROM sift.check('ev');

-- a TTL: the part that is in the store is dropped whole and its object stays for the grace of the standbys; the part with the recent rows is kept
SELECT sift.set_ttl('ev', 'ts', interval '30 days') AS ttl;
SELECT sift.set_ttl('ev', 'ts', interval '30 days') AS ttl;
SELECT relid::regclass AS relation, col, retention FROM sift.ttl_policies;

-- the policies are filed under the OID of the table: a rename keeps them, and setting them again finds them
ALTER TABLE ev RENAME TO ev_new;
SELECT relid::regclass AS relation, move_after FROM sift.tier_policies;
SELECT relid::regclass AS relation, col, retention FROM sift.ttl_policies;
SELECT sift.set_tier_policy('ev_new', 'file://' || :'tierdir', '45 days') AS changed;
SELECT sift.set_ttl('ev_new', 'ts', interval '30 days') AS ttl;
SELECT (SELECT count(*) FROM sift.tier_policies) AS tier_policies, (SELECT count(*) FROM sift.ttl_policies) AS ttl_policies;
ALTER TABLE ev_new RENAME TO ev;

-- a B-tree built before the drop keeps the entries of the dropped rows
CREATE UNIQUE INDEX ev_id ON ev (id);
VACUUM ev;
SELECT count(*) AS rows, min(ts) >= localtimestamp - interval '31 days' AS only_recent FROM ev;
SELECT count(*) AS leaving, bool_and(expires_at > now()) AS object_kept FROM sift.tier_status;
SELECT count(*) AS problems FROM sift.check('ev');

-- a fetch by one of those entries is "no row", for a read, an UPDATE, a DELETE and the check of a unique key; the rows that stayed are found
CREATE FUNCTION uses_btree(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Index.*ev_id' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET sift.enable_agg = off;
SELECT uses_btree('SELECT count(*) FROM ev WHERE id BETWEEN 1 AND 5000') AS range_via_btree, uses_btree('SELECT count(*) FROM ev WHERE id = 42') AS point_via_btree;
SELECT count(*) AS stale_range FROM ev WHERE id BETWEEN 1 AND 5000;
SELECT count(*) AS stale_point FROM ev WHERE id = 42;
SELECT count(*) AS live_range FROM ev WHERE id > 5000;
UPDATE ev SET v = 0 WHERE id = 43;
DELETE FROM ev WHERE id = 44;
SELECT count(*) AS rows FROM ev;
BEGIN;
INSERT INTO ev VALUES (42, localtimestamp, 0, 0, 'again');
SELECT count(*) AS reinserted FROM ev WHERE id = 42;
ROLLBACK;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET sift.enable_agg;

SELECT sift.set_ttl('ev', 'ts', NULL) AS cleared;
SELECT sift.set_ttl('ev', 'ts', NULL) AS cleared;
SELECT count(*) AS policies FROM sift.ttl_policies;

-- a dropped table takes its policies with it
CREATE TABLE gone (ts timestamp NOT NULL) USING sift_table;
SELECT sift.set_tier_policy('gone', 'file://' || :'tierdir') AS tiered;
SELECT sift.set_ttl('gone', 'ts', interval '1 day') AS ttl;
DROP TABLE gone;
SELECT (SELECT count(*) FROM sift.tier_policies) AS tier_policies, (SELECT count(*) FROM sift.ttl_policies) AS ttl_policies;

-- a policy cannot be cleared while an object of the table waits to be deleted
SELECT sift.clear_tier_policy('ev');
DROP TABLE ev, evn, plain;
\set rm 'rm -rf ' :tierdir ' ' :tierdir '.bak'
COPY (SELECT 1) TO PROGRAM :'rm';
DROP FUNCTION uses_btree(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
