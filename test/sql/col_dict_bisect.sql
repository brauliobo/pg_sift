-- A dictionary of thousands of strings is searched in the stream (a bisection of its sorted order, a block of strings decompressed for each step), and the backend keeps
-- the positions it looked at (col/dictcache.c, the bisection cache) under the segment's generation, so that the next search does not read them again, and parses the
-- dictionary only when the searches have cost what the parse would. The answers never depend on it: the same keys, the missing ones, a range and a prefix,
-- before and after a promotion, a merge, a VACUUM, a delete and a REINDEX, all in one backend.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

CREATE TABLE bs (id int, s text) WITH (autovacuum_enabled = off);
INSERT INTO bs SELECT g, 'key ' || md5(g::text) FROM generate_series(1, 30000) g;
CREATE INDEX bs_idx ON bs USING sift_col (id sift.col_int4_ops, s sift.col_text_ops) WITH (point_columns = 's', write_buffer_limit = 4096);
SELECT count(*) > 0 AS has_segment FROM sift.col_segments('bs_idx'::regclass) WHERE kind = 'immutable';

-- (few searches in the first stages, so that the cache is read, not the parsed dictionary the many searches bring)
-- the rows of the index for a key, a range or a prefix against the heap's, for keys that are there (some of every part of the order) and keys that are not
CREATE FUNCTION bs_check(tag text, m int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	k text;
	op text;
	got bigint;
	want bigint;
	bad int := 0;
	n int := 0;
BEGIN
	FOR k IN SELECT 'key ' || md5((1 + (g * 7919) % 33000)::text) FROM generate_series(1, m) g
			 UNION ALL SELECT 'key ' || md5((30000 + g * 37)::text) FROM generate_series(1, m) g
			 UNION ALL SELECT 'key ' || substr(md5(g::text), 1, 5 + g % 20) FROM generate_series(1, m / 6 + 1) g
			 UNION ALL SELECT * FROM (VALUES (''), ('a'), ('key '), ('key 0'), ('key f'), ('key ffffffffffffffffffffffffffffffff'), ('zzz'), ('key ' || md5('1'))) v WHERE m > 5 LOOP
		FOREACH op IN ARRAY ARRAY['eq', 'lt', 'ge', 'prefix'] LOOP
			SELECT rows INTO got FROM sift._test_col_counters('bs_idx', '1:' || op || ':' || k);
			SELECT count(*) INTO want FROM (SELECT s FROM bs OFFSET 0) x
			 WHERE CASE op WHEN 'eq' THEN s = k WHEN 'lt' THEN s < k COLLATE "C" WHEN 'ge' THEN s >= k COLLATE "C" ELSE left(s, length(k)) = k END;
			n := n + 1;
			IF got IS DISTINCT FROM want THEN
				bad := bad + 1;
				RAISE NOTICE '% % %: index %, heap %', tag, op, k, got, want;
			END IF;
		END LOOP;
	END LOOP;
	RETURN tag || ': ' || n || ' searches, ' || bad || ' differ';
END $$;

SELECT count(*) > 0 AS reset FROM sift._test_col_dict_cache(true);
SELECT bs_check('first', 1);
SELECT bytes > 0 AS positions_kept FROM sift._test_col_dict_cache(false);
SELECT bs_check('warm', 1);

-- written again: promotion of new rows, a merge, a delete and VACUUM, REINDEX: every one is a segment the cache has not seen, or one that it has, under another generation
INSERT INTO bs SELECT g, 'key ' || md5(g::text) FROM generate_series(30001, 36000) g;
SELECT sift.col_promote('bs_idx') >= 0 AS promoted;
SELECT bs_check('promoted', 1);
SELECT sift.col_merge('bs_idx', true) >= 0 AS merged;
SELECT bs_check('merged', 1);
DELETE FROM bs WHERE id % 7 = 0;
VACUUM bs;
SELECT bs_check('vacuumed', 1);
INSERT INTO bs SELECT g, 'key ' || md5(g::text) FROM generate_series(1, 40000) g WHERE g % 7 = 0 AND g > 36000;
SELECT sift.col_promote('bs_idx') >= 0 AS promoted_again;
SELECT bs_check('promoted again', 1);
REINDEX INDEX bs_idx;
SELECT bs_check('reindexed', 1);
UPDATE bs SET s = 'key ' || md5((id + 1)::text) WHERE id % 11 = 0;
VACUUM bs;
SELECT sift.col_merge('bs_idx', true) >= 0 AS merged_again;
SELECT bs_check('updated', 1);
SELECT sift.col_verify('bs_idx', true);

-- many searches in one backend: the cache hands over to the parsed dictionary
SELECT bs_check('many', 120);

-- a cache of no room for the positions (one kilobyte): the same answers
SET sift.test_limits = 'dict_cache_kb=1';
SELECT bs_check('no room', 30);
RESET sift.test_limits;

DROP FUNCTION bs_check(text, int);
DROP TABLE bs;
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
