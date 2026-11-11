-- Compaction of a shared value store (docs/COL-VSTORE.md): a new epoch begins, segments written after it name it, and the old epoch is retired when no segment of any
-- member names it. After every step each shape answers as the heap does, whichever epoch the segments of the members name.
SET client_min_messages = warning;
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET sift.async_maintenance = off;
SET sift.test_vstore_columns = 'url';

CREATE TABLE ve (id int PRIMARY KEY, tenant int, url text);
INSERT INTO ve SELECT i, i % 5, 'https://example.com/p/' || md5(i::text) FROM generate_series(1, 4000) i;
ANALYZE ve;
CREATE INDEX ve_a ON ve USING sift_col (id, tenant, url);
CREATE INDEX ve_b ON ve USING sift_col (id, url);

CREATE TABLE ve_q (n serial, q text);
INSERT INTO ve_q (q) VALUES
 ('SELECT count(*), sum(id) FROM ve WHERE url = ''https://example.com/p/'' || md5(''77'')'),
 ('SELECT count(*), sum(id) FROM ve WHERE url = ''https://example.com/q/'' || md5(''77'')'),
 ('SELECT count(*), sum(id) FROM ve WHERE url IN (''https://example.com/p/'' || md5(''3000''), ''https://example.com/q/'' || md5(''5''), ''none'')'),
 ('SELECT count(*), sum(id) FROM ve WHERE url > ''https://example.com/p/5'' AND url < ''https://example.com/q/'''),
 ('SELECT count(*), count(DISTINCT url), min(url), max(url) FROM ve'),
 ('SELECT md5(string_agg(g::text, '','' ORDER BY g)) FROM (SELECT count(*) || ''/'' || url AS g FROM ve GROUP BY url) z'),
 ('SELECT md5(string_agg(g::text, '','' ORDER BY g)) FROM (SELECT count(*) || ''/'' || url AS g FROM ve WHERE tenant = 2 GROUP BY url) z');

CREATE FUNCTION ve_check(label text) RETURNS TABLE (step text, shapes int, same int) LANGUAGE plpgsql AS $$
DECLARE r record; a text; b text; cnt int := 0; ok int := 0;
BEGIN
	FOR r IN SELECT q FROM ve_q ORDER BY ve_q.n LOOP
		SET LOCAL sift.enable_agg = on;
		EXECUTE r.q INTO a;
		SET LOCAL sift.enable_agg = off;
		EXECUTE r.q INTO b;
		cnt := cnt + 1; ok := ok + (a IS NOT DISTINCT FROM b)::int;
	END LOOP;
	RETURN QUERY SELECT label, cnt, ok;
END $$;

-- what the store holds and what the segments name, by epoch: the epoch of the store, its runs, the codes in use of the current epoch, and for each epoch the members that name it
CREATE FUNCTION ve_state(label text) RETURNS TABLE (step text, epoch int8, runs int4, all_runs int4, codes int8, live int8, naming text) LANGUAGE sql AS $$
	SELECT label, i.epoch, i.runs, i.all_runs, i.view_values,
	       (SELECT u.codes FROM sift.col_value_store_usage('ve_url_sift_values') u WHERE u.member IS NULL AND u.epoch = i.epoch),
	       (SELECT coalesce(string_agg(u.epoch || ':' || u.member::text, ' ' ORDER BY u.epoch, u.member::text), '-')
	          FROM sift.col_value_store_usage('ve_url_sift_values') u WHERE u.member IS NOT NULL)
	  FROM sift._test_vstore_info('ve_url_sift_values') i $$;

SELECT * FROM ve_check('built'), ve_state('built');

-- a thousand values replaced: their old codes are garbage once the segments that held them are merged
UPDATE ve SET url = 'https://example.com/q/' || md5(id::text) WHERE id <= 1000;
SELECT sift.col_promote('ve_a') > 0 AS promoted_a, sift.col_promote('ve_b') > 0 AS promoted_b;
VACUUM ve;
SELECT sift.col_merge('ve_a', true) >= 0 AS merged_a, sift.col_merge('ve_b', true) >= 0 AS merged_b;
SELECT * FROM ve_check('churned'), ve_state('churned');
SELECT total_codes, live_codes, garbage_pct FROM sift.col_value_store_status WHERE table_name = 've'::regclass;

-- a new epoch begins: nothing is copied, the segments still name the old one and are read from its runs
SELECT sift._col_value_store_begin('ve', 'url') = 've_url_sift_values'::regclass AS began;
SELECT * FROM ve_check('a new epoch, segments of the old one'), ve_state('a new epoch, segments of the old one');
SELECT sift._col_value_store_end('ve_url_sift_values') AS removed_while_named;
SELECT * FROM ve_check('the old epoch is still named'), ve_state('the old epoch is still named');

-- rows written now name the new epoch; a query reads segments of both
INSERT INTO ve SELECT 10000 + i, i % 5, 'https://example.com/n/' || md5(i::text) FROM generate_series(1, 500) i;
SELECT sift.col_promote('ve_a') > 0 AS promoted_a;
SELECT * FROM ve_check('segments of both epochs'), ve_state('segments of both epochs');
SELECT sift._test_vstore_forget();
SELECT * FROM ve_check('segments of both epochs, another backend state');

-- the compaction: the segments of the members that name the old epoch are rewritten, the old epoch is retired (marked now, removed when the horizon has passed)
SELECT sift.col_value_store_compact('ve', 'url') AS removed_now;
SELECT * FROM ve_check('compacted'), ve_state('compacted');
-- the old epoch can go when the transactions that began before it was marked have ended: a transaction later
SELECT pg_current_xact_id() IS NOT NULL AS a_transaction_later;
SELECT sift._col_value_store_end('ve_url_sift_values') AS removed_later;
SELECT * FROM ve_check('the old epoch removed'), ve_state('the old epoch removed');
SELECT total_codes, live_codes, garbage_pct FROM sift.col_value_store_status WHERE table_name = 've'::regclass;
SELECT sift._test_vstore_forget();
SELECT * FROM ve_check('the old epoch removed, another backend state');
SELECT sift.col_verify('ve_a', true), sift.col_verify('ve_b', true);

-- nothing to free: no new epoch
SELECT sift.col_value_store_compact('ve', 'url') AS nothing_to_free;
SELECT * FROM ve_state('nothing to free');

-- VACUUM begins a new epoch by itself when half the codes are named by no segment (sift.test_vstore_gc_kb: the store size from which it looks; 8 MB by default)
UPDATE ve SET url = 'https://example.com/r/' || md5(id::text);
SELECT sift.col_promote('ve_a') > 0 AS promoted_a, sift.col_promote('ve_b') > 0 AS promoted_b;
SET sift.test_vstore_gc_kb = 0;
VACUUM ve;
SELECT sift.col_merge('ve_a', true) >= 0 AS merged_a, sift.col_merge('ve_b', true) >= 0 AS merged_b;
SELECT * FROM ve_check('replaced, vacuumed, merged'), ve_state('replaced, vacuumed, merged');
SET client_min_messages = notice;
VACUUM ve;
SET client_min_messages = warning;
SELECT * FROM ve_check('vacuumed: a new epoch'), ve_state('vacuumed: a new epoch');
-- the rewrite of a member that names the old epoch moves its strings to the new one (what the maintenance of the index does by itself, as the VACUUM asks)
SELECT sift.col_maintain('ve_a') AS rewrote_a, sift.col_maintain('ve_b') AS rewrote_b;
SELECT * FROM ve_check('rewritten into the new epoch'), ve_state('rewritten into the new epoch');
SELECT sift.col_maintain('ve_a') AS nothing_more_for_a;
VACUUM ve;
SELECT * FROM ve_state('the old epoch marked');
SELECT pg_current_xact_id() IS NOT NULL AS a_transaction_later;
VACUUM ve;
SELECT * FROM ve_check('the old epoch removed by VACUUM'), ve_state('the old epoch removed by VACUUM');
SELECT total_codes, live_codes, garbage_pct FROM sift.col_value_store_status WHERE table_name = 've'::regclass;
RESET sift.test_vstore_gc_kb;

DROP TABLE ve_q;
DROP FUNCTION ve_check(text), ve_state(text);
DROP TABLE ve;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
