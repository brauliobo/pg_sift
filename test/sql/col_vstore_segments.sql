-- A leaf of a segment whose strings are in a shared value store (docs/COL-VSTORE.md) holds references to them: every answer is the heap's, and the segment
-- the unshared index writes, for text and uuid leaves, equality, IN, range, prefix, group by, NULLs and the empty string, after the build, the write
-- buffer, a promotion, a merge, VACUUM and REINDEX, and a second backend finds the same strings.
SET client_min_messages = warning;
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET sift.async_maintenance = off;
SET sift.test_vstore_columns = 'url,uid';

CREATE TABLE vg (id int PRIMARY KEY, tenant int, uid uuid, url text);
INSERT INTO vg SELECT i, i % 7, CASE WHEN i % 31 = 0 THEN NULL ELSE md5((i % 3000)::text)::uuid END,
	CASE WHEN i % 29 = 0 THEN NULL WHEN i % 43 = 0 THEN '' ELSE 'https://example.com/p/' || md5((i % 4000)::text) END FROM generate_series(1, 12000) i;
CREATE INDEX vg_a ON vg USING sift_col (id, tenant, uid, url) WITH (write_buffer_limit = 256);
CREATE INDEX vg_b ON vg USING sift_col (url, id);
SELECT store, indexes_using FROM sift.col_value_store_status ORDER BY 1;
SELECT max(format) AS format FROM sift.col_segments('vg_a') WHERE kind = 'immutable';

-- the shapes: every one is run on the index and on the heap
CREATE TABLE vg_q (n serial, q text);
INSERT INTO vg_q (q) VALUES
 ('SELECT count(*), sum(id) FROM vg WHERE url = ''https://example.com/p/'' || md5(''77'')'),
 ('SELECT count(*), sum(id) FROM vg WHERE url IN (''https://example.com/p/'' || md5(''1''), ''https://example.com/p/'' || md5(''3999''), '''', ''none'')'),
 ('SELECT count(*), sum(id) FROM vg WHERE url > ''https://example.com/p/5'' AND url < ''https://example.com/p/6'''),
 ('SELECT count(*), sum(id) FROM vg WHERE url LIKE ''https://example.com/p/ab%'''),
 ('SELECT count(*), sum(id) FROM vg WHERE url = '''''),
 ('SELECT count(*), sum(id) FROM vg WHERE url IS NULL'),
 ('SELECT count(*), sum(id) FROM vg WHERE uid = md5(''5'')::uuid'),
 ('SELECT count(*), sum(id) FROM vg WHERE uid IN (md5(''1'')::uuid, md5(''2999'')::uuid, md5(''x'')::uuid)'),
 ('SELECT count(*), sum(id) FROM vg WHERE uid >= md5(''7'')::uuid AND tenant = 3'),
 ('SELECT count(*), count(DISTINCT url), min(url), max(url) FROM vg'),
 ('SELECT count(*), count(DISTINCT uid) FROM vg WHERE tenant < 4'),
 ('SELECT md5(string_agg(g::text, '','' ORDER BY g)) FROM (SELECT count(*) || ''/'' || url AS g FROM vg GROUP BY url) z'),
 ('SELECT md5(string_agg(g::text, '','' ORDER BY g)) FROM (SELECT count(*) || ''/'' || uid AS g FROM vg WHERE tenant = 2 GROUP BY uid) z');

CREATE FUNCTION vg_check(label text) RETURNS TABLE (step text, shapes int, same int) LANGUAGE plpgsql AS $$
DECLARE r record; a text; b text; cnt int := 0; ok int := 0;
BEGIN
	FOR r IN SELECT q FROM vg_q ORDER BY vg_q.n LOOP
		SET LOCAL sift.enable_agg = on;
		EXECUTE r.q INTO a;
		SET LOCAL sift.enable_agg = off;
		EXECUTE r.q INTO b;
		cnt := cnt + 1; ok := ok + (a IS NOT DISTINCT FROM b)::int;
	END LOOP;
	RETURN QUERY SELECT label, cnt, ok;
END $$;

SELECT * FROM vg_check('built');
SELECT sift._test_vstore_forget();
SELECT * FROM vg_check('another backend state');

-- written again: the write buffer, deletes and updates, a promotion, a merge, VACUUM, REINDEX
INSERT INTO vg SELECT 20000 + i, i % 7, md5((i % 500 + 5000)::text)::uuid, 'https://example.com/p/' || md5((i % 900 + 4000)::text) FROM generate_series(1, 3000) i;
SELECT * FROM vg_check('write buffer');
DELETE FROM vg WHERE id % 11 = 0 AND id < 12000;
UPDATE vg SET url = 'https://example.com/p/' || md5((id + 100000)::text) WHERE id % 13 = 0 AND id < 12000;
SELECT sift.col_promote('vg_a') > 0 AS promoted;
SELECT * FROM vg_check('promoted');
INSERT INTO vg SELECT 40000 + i, i % 7, md5((i % 100)::text)::uuid, 'https://example.com/p/' || md5((i % 100 + 9000)::text) FROM generate_series(1, 3000) i;
SELECT sift.col_promote('vg_a') > 0 AS promoted;
SELECT sift.col_merge('vg_a', true) >= 0 AS merged;
SELECT * FROM vg_check('merged');
VACUUM vg;
SELECT * FROM vg_check('vacuumed');
SELECT sift.col_verify('vg_a', true);
SELECT sift.col_verify('vg_b', true);
REINDEX INDEX vg_a;
SELECT * FROM vg_check('reindexed');
SELECT max(format) AS format FROM sift.col_segments('vg_a') WHERE kind = 'immutable';

-- the off switch: a column format minor below 15 writes strings in the segments
SET sift.col_format_minor = 14;
CREATE INDEX vg_c ON vg USING sift_col (id, url);
RESET sift.col_format_minor;
SELECT max(format) AS format FROM sift.col_segments('vg_c') WHERE kind = 'immutable';
SELECT * FROM vg_check('with an index of its own strings');

DROP TABLE vg_q;
DROP FUNCTION vg_check(text);
DROP TABLE vg;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
