-- Garbage in a shared value store goes by itself: rows replaced round after round leave codes that no segment names, VACUUM begins a new epoch when half of them
-- are such and the store is large enough, the maintenance worker rewrites the indexes that name the old epoch, and a later VACUUM removes it. The blocks of the removed
-- epoch are used again by the next rounds (the store does not grow for ever). The test clusters run with the worker off: this database turns it on.
CREATE EXTENSION pg_sift;
DO $$ BEGIN
	EXECUTE format('ALTER DATABASE %I SET sift.async_maintenance = on', current_database());
	EXECUTE format('ALTER DATABASE %I SET sift.col_auto_merge = on', current_database());
	EXECUTE format('ALTER DATABASE %I SET sift.test_vstore_gc_kb = 0', current_database());
END $$;
\c
SET client_min_messages = warning;

CREATE TABLE ch (id int PRIMARY KEY, url text);
INSERT INTO ch SELECT i, 'https://example.com/p/' || md5(i::text) || md5((i * 7)::text) FROM generate_series(1, 20000) i;
ANALYZE ch;
CREATE INDEX ch_a ON ch USING sift_col (id, url);
CREATE INDEX ch_b ON ch USING sift_col (url, id);
REINDEX INDEX ch_a;

CREATE FUNCTION ch_check() RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	SELECT count(*) || ':' || md5(string_agg(url, ',' ORDER BY url)) INTO a FROM ch WHERE url > 'https://example.com/p/5';
	SET LOCAL sift.enable_agg = off;
	SELECT count(*) || ':' || md5(string_agg(url, ',' ORDER BY url)) INTO b FROM ch WHERE url > 'https://example.com/p/5';
	RETURN a = b;
END $$;

-- one round: every row gets a new value (the VACUUM that follows drops the dead rows from the segments, and with them the use of their strings)
CREATE FUNCTION ch_round(r int) RETURNS void LANGUAGE sql AS $$
	UPDATE ch SET url = 'https://example.com/r' || r || '/' || md5(id::text) || md5((id * 11 + r)::text) $$;

-- true when the store has no codes that no segment names (the worker rewrote the members and VACUUM retired the epoch it left), within 30 s
CREATE FUNCTION ch_compacted() RETURNS boolean LANGUAGE plpgsql AS $$
BEGIN
	FOR i IN 1..150 LOOP
		IF (SELECT live_codes = total_codes AND total_codes > 0 FROM sift.col_value_store_status WHERE table_name = 'ch'::regclass)
		   AND NOT EXISTS (SELECT 1 FROM sift.col_value_store_usage((SELECT store FROM sift.col_value_store_status WHERE table_name = 'ch'::regclass)) u WHERE u.epoch < (SELECT max(epoch) FROM sift.col_value_store_usage((SELECT store FROM sift.col_value_store_status WHERE table_name = 'ch'::regclass)))) THEN
			RETURN true;
		END IF;
		PERFORM pg_sleep(0.2);
	END LOOP;
	RETURN false;
END $$;

SELECT ch_round(1);
VACUUM ch;
SELECT garbage_pct >= 50 AS half_is_garbage FROM sift.col_value_store_status WHERE table_name = 'ch'::regclass;
SET client_min_messages = notice;
VACUUM ch;
SET client_min_messages = warning;
VACUUM ch;
SELECT pg_sleep(1);
VACUUM ch;
SELECT pg_sleep(1);
VACUUM ch;
SELECT ch_compacted() AS compacted_by_the_worker;
VACUUM ch;
SELECT pg_current_xact_id() IS NOT NULL AS a_transaction_later;
VACUUM ch;
SELECT ch_check() AS answers_as_the_normal_plan;
SELECT total_codes = live_codes AS no_garbage, garbage_pct FROM sift.col_value_store_status WHERE table_name = 'ch'::regclass;
CREATE TABLE ch_size AS SELECT pg_relation_size(store) AS bytes FROM sift.col_value_store_status WHERE table_name = 'ch'::regclass;

-- one more round: the store stays about as large
SELECT ch_round(2);
VACUUM ch;
VACUUM ch;
VACUUM ch;
SELECT pg_sleep(1);
VACUUM ch;
SELECT pg_sleep(1);
VACUUM ch;
SELECT ch_compacted() AS compacted_again;
VACUUM ch;
SELECT pg_current_xact_id() IS NOT NULL AS a_transaction_later;
VACUUM ch;
SELECT ch_check() AS answers_as_the_normal_plan;
SELECT pg_relation_size(store) <= 2 * (SELECT bytes FROM ch_size) AS store_reuses_its_blocks
  FROM sift.col_value_store_status WHERE table_name = 'ch'::regclass;
SELECT sift.col_verify('ch_a', true), sift.col_verify('ch_b', true);

DROP FUNCTION ch_check(), ch_round(int), ch_compacted();
DROP TABLE ch, ch_size;
DO $$ BEGIN
	EXECUTE format('ALTER DATABASE %I RESET sift.async_maintenance', current_database());
	EXECUTE format('ALTER DATABASE %I RESET sift.col_auto_merge', current_database());
	EXECUTE format('ALTER DATABASE %I RESET sift.test_vstore_gc_kb', current_database());
END $$;
DROP EXTENSION pg_sift;
