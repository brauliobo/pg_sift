-- Rows of a Format 2.0 sift_table that has an index are read by address from the row pack of their part (SEC_ROW_PACK, blocks of 64 rows) through the address hash (SEC_TID_HASH, version 2): every row is
-- the one of the heap twin, for NULLs, strings over 128 bytes and values the column keeps by reference (cells the pack leaves to SEC_GRANULES), after ALTER TABLE ADD, DROP and ALTER COLUMN TYPE, with
-- column deltas patching a column, with deleted rows, in the transaction that deletes them and after VACUUM FULL; a table that has no index gets neither section, and a part whose pack would be
-- over 40% of its granules gets no pack.
-- requires: build col
CREATE EXTENSION pg_sift_test CASCADE;
DO $$ BEGIN PERFORM set_config('sift.test_create_format1', 'off', false); EXCEPTION WHEN OTHERS THEN NULL; END $$;
CREATE FUNCTION rp_parts(regclass) RETURNS text LANGUAGE C AS 'pg_sift', 'sift_test_f2_row_pack_parts';
CREATE FUNCTION rp_stats(bool) RETURNS text LANGUAGE C AS 'pg_sift', 'sift_test_f2_row_pack_stats';
CREATE FUNCTION rp_policy(int) RETURNS int LANGUAGE C AS 'pg_sift', 'sift_test_f2_row_pack_policy';
CREATE FUNCTION hash_stats(bool) RETURNS text LANGUAGE C AS 'pg_sift', 'sift_test_f2_tid_hash_stats';

-- the sections of the parts of a table: how many have the hash and the pack (ext_flags bits 6 and 7), and the bytes of each
CREATE FUNCTION sections(tbl regclass, OUT parts int, OUT hashed int, OUT packed int, OUT hash_bytes bigint, OUT pack_bytes bigint, OUT granule_bytes bigint) LANGUAGE sql AS $$
	SELECT count(*)::int, count(*) FILTER (WHERE (split_part(l, ' ', 2)::int & 64) <> 0)::int, count(*) FILTER (WHERE (split_part(l, ' ', 2)::int & 128) <> 0)::int,
		coalesce(sum(split_part(l, ' ', 5)::bigint), 0), coalesce(sum(split_part(l, ' ', 6)::bigint), 0), coalesce(sum(split_part(l, ' ', 4)::bigint), 0)
	FROM regexp_split_to_table(rp_parts(tbl), E'\n') l WHERE l <> ''
$$;

CREATE FUNCTION rows_md5(tbl text, pred text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT md5(rw::text) FROM %I rw WHERE %s', tbl, pred) INTO r;
	RETURN r;
END $$;

-- the rows of the table and of its twin, one lookup of the key each: the same text
CREATE FUNCTION same_rows(a text, b text, lo int, hi int, step int) RETURNS text LANGUAGE sql AS $$
	SELECT count(*) || ' lookups, ' || count(*) FILTER (WHERE rows_md5(a, 'id = ' || g) IS NOT DISTINCT FROM rows_md5(b, 'id = ' || g)) || ' same as the twin, '
		|| count(*) FILTER (WHERE rows_md5(b, 'id = ' || g) IS NOT NULL) || ' present'
	FROM generate_series(lo, hi, step) g
$$;

CREATE TABLE rp (id int PRIMARY KEY, a int, b bigint, f float8, r float4, t text, w text, n numeric(10,2), d date, ts timestamp, flag bool, u uuid) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE rp_u (LIKE rp) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE rp_h (LIKE rp);
ALTER TABLE rp_h ADD PRIMARY KEY (id);
CREATE TEMP TABLE src AS
SELECT g AS id, CASE WHEN g % 7 = 0 THEN NULL ELSE g % 1000 END AS a, g * 1000003::bigint AS b, CASE WHEN g % 11 = 0 THEN NULL ELSE sin(g) * 1000 END AS f, (g % 100)::float4 / 4 AS r,
	CASE WHEN g % 13 = 0 THEN NULL ELSE 'name-' || (g * 31 % 997) END AS t,
	CASE WHEN g % 50 = 0 THEN repeat(md5(g::text), 80) WHEN g % 10 = 0 THEN repeat(md5(g::text), 5) WHEN g % 17 = 0 THEN '' ELSE 'w' || (g % 9) END AS w,
	CASE WHEN g % 19 = 0 THEN NULL ELSE (g % 100000)::numeric / 100 END AS n, date '2020-01-01' + g % 2000 AS d, timestamp '2024-01-01' + g * interval '17 seconds' AS ts, g % 3 = 0 AS flag,
	CASE WHEN g % 23 = 0 THEN NULL ELSE md5(g::text)::uuid END AS u
FROM generate_series(1, 20000) g;
SELECT rp_policy(100000) = 40 AS policy_forced;
INSERT INTO rp SELECT * FROM src;
INSERT INTO rp_u SELECT * FROM src;
INSERT INTO rp_h SELECT * FROM src;
SELECT rp_policy(40) = 100000 AS policy_restored;
SELECT sift.table_maintain('rp') IS NOT NULL AS maintained, sift.table_maintain('rp_u') IS NOT NULL AS maintained_unindexed;
ANALYZE rp;
ANALYZE rp_u;
ANALYZE rp_h;

-- the indexed table has the sections in its parts, the table with no index has none
SELECT parts > 0 AS has_parts, hashed = parts AS every_part_hashed, packed = parts AS every_part_packed, hash_bytes > 0 AS hash_bytes, pack_bytes > 0 AS pack_bytes FROM sections('rp');
SELECT parts > 0 AS has_parts, hashed AS hashed, packed AS packed, hash_bytes AS hash_bytes, pack_bytes AS pack_bytes FROM sections('rp_u');

SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT rp_stats(true) IS NOT NULL AS reset;
SELECT hash_stats(true) IS NOT NULL AS reset;
SELECT same_rows('rp', 'rp_h', 1, 20000, 23) AS parity_of_lookups;
SELECT split_part(s, ' ', 2)::int >= 800 AS rows_came_from_the_pack, split_part(s, ' ', 3)::int > 0 AS wide_cells_came_from_the_granules FROM rp_stats(false) s;
SELECT split_part(s, ' ', 1)::int >= 800 AS used_the_hash, split_part(s, ' ', 2)::float8 / split_part(s, ' ', 1)::int <= 1.1 AS about_one_row_a_lookup FROM hash_stats(false) s;
-- the leading columns alone, and one column alone
SELECT (SELECT count(*) FROM generate_series(1, 20000, 29) g JOIN rp f ON f.id = g AND f.a IS NOT DISTINCT FROM (SELECT a FROM rp_h WHERE id = g)) AS leading_attribute_same,
	(SELECT count(*) FROM generate_series(1, 20000, 29) g JOIN rp f ON f.id = g AND f.u IS NOT DISTINCT FROM (SELECT u FROM rp_h WHERE id = g)) AS last_attribute_same,
	(SELECT count(*) FROM generate_series(1, 20000, 29) g JOIN rp f ON f.id = g AND f.w IS NOT DISTINCT FROM (SELECT w FROM rp_h WHERE id = g)) AS wide_attribute_same;

-- the same through the other table: a secondary index
CREATE INDEX rp_a ON rp (a);
CREATE INDEX rp_h_a ON rp_h (a);
SELECT count(*) AS secondary_rows, md5(string_agg(rw::text, '|' ORDER BY id)) = (SELECT md5(string_agg(rw::text, '|' ORDER BY id)) FROM rp_h rw WHERE a BETWEEN 100 AND 130) AS same_as_heap FROM rp rw WHERE a BETWEEN 100 AND 130;

-- column deltas: a column patched in place is read from the pack with the patch laid over it
UPDATE rp SET a = a + 1, t = 'patched ' || id WHERE id % 97 = 0;
UPDATE rp_h SET a = a + 1, t = 'patched ' || id WHERE id % 97 = 0;
SELECT same_rows('rp', 'rp_h', 1, 20000, 17) AS after_column_deltas;
SELECT same_rows('rp', 'rp_h', 97, 20000, 97) AS patched_rows;

-- deleted rows: the vector hides them, the row the pack still holds is not returned
DELETE FROM rp WHERE id % 101 = 0;
DELETE FROM rp_h WHERE id % 101 = 0;
SELECT same_rows('rp', 'rp_h', 1, 20000, 19) AS after_deletes;
SELECT same_rows('rp', 'rp_h', 101, 20000, 101) AS deleted_rows;
BEGIN;
DELETE FROM rp WHERE id = 500;
SELECT count(*) AS deleted_in_this_transaction FROM rp WHERE id = 500;
ROLLBACK;
SELECT count(*) AS back_after_rollback FROM rp WHERE id = 500;

-- ALTER TABLE: the part keeps the columns it was written with
ALTER TABLE rp ADD COLUMN extra int DEFAULT 7;
ALTER TABLE rp_h ADD COLUMN extra int DEFAULT 7;
ALTER TABLE rp ADD COLUMN late text;
ALTER TABLE rp_h ADD COLUMN late text;
SELECT same_rows('rp', 'rp_h', 1, 20000, 31) AS after_add_column;
ALTER TABLE rp DROP COLUMN b;
ALTER TABLE rp_h DROP COLUMN b;
ALTER TABLE rp DROP COLUMN r;
ALTER TABLE rp_h DROP COLUMN r;
SELECT same_rows('rp', 'rp_h', 1, 20000, 31) AS after_drop_column;
ALTER TABLE rp ALTER COLUMN a TYPE bigint;
ALTER TABLE rp_h ALTER COLUMN a TYPE bigint;
SELECT same_rows('rp', 'rp_h', 1, 20000, 31) AS after_alter_type;

-- VACUUM FULL writes the table again
VACUUM FULL rp;
SELECT same_rows('rp', 'rp_h', 1, 20000, 31) AS after_vacuum_full;
SELECT sift.table_maintain('rp') IS NOT NULL AS maintained_again;
SELECT same_rows('rp', 'rp_h', 1, 20000, 7) AS after_maintain;
RESET enable_seqscan;
RESET enable_bitmapscan;

-- a part whose pack is over 40% of its granules gets none (a table of narrow, well coded columns), the hash is still there
CREATE TABLE rp_n (id int PRIMARY KEY, c int, s text) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO rp_n SELECT g, g % 5, 'status ' || (g % 4) FROM generate_series(1, 20000) g;
SELECT sift.table_maintain('rp_n') IS NOT NULL AS maintained;
SELECT parts > 0 AS has_parts, hashed AS hashed, packed AS packed FROM sections('rp_n');

DROP FUNCTION same_rows(text, text, int, int, int), rows_md5(text, text), sections(regclass), rp_parts(regclass), rp_stats(bool), rp_policy(int), hash_stats(bool);
DROP TABLE rp, rp_u, rp_h, rp_n;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
