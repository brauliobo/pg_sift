-- A dictionary of 1,024 strings or more that a plain index keeps in compressed blocks (ids, hashes and uuids: no leaf asked for its order) carries the hash of its
-- strings to their codes too, so that the first equality of a backend finds its code in a few pages, not by parsing the
-- whole dictionary. Equality, IN, prefix and range tests give the heap's answer on it (an equality reads the hash, any other test parses the dictionary), across
-- segments, after a promotion, a merge, a VACUUM and a REINDEX; col_verify checks every slot; dict_lookup = off writes none.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.async_maintenance = off;

CREATE FUNCTION db_cmp(idx regclass, preds text, heapwhere text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a bigint; b bigint; ex bigint; mi bigint;
BEGIN
	SELECT count(*) INTO a FROM sift._test_col_tids(idx, preds);
	EXECUTE format('SELECT count(*) FROM db WHERE %s', heapwhere) INTO b;
	EXECUTE format('SELECT count(*) FROM (SELECT tid FROM sift._test_col_tids(%L, %L) EXCEPT SELECT ctid FROM db WHERE %s) z', idx, preds, heapwhere) INTO ex;
	EXECUTE format('SELECT count(*) FROM (SELECT ctid FROM db WHERE %s EXCEPT SELECT tid FROM sift._test_col_tids(%L, %L)) z', heapwhere, idx, preds) INTO mi;
	RETURN CASE WHEN a = b AND ex = 0 AND mi = 0 THEN 'ok' ELSE format('MISMATCH index=%s heap=%s extra=%s missing=%s', a, b, ex, mi) END;
END $$;

CREATE TABLE db (id int, s text, u uuid);
INSERT INTO db SELECT i, 'k' || md5(i::text), md5((i % 4000)::text)::uuid FROM generate_series(1, 6000) i;
CREATE INDEX db_idx ON db USING sift_col (s sift.col_text_ops, u sift.col_uuid_ops, id sift.col_int4_ops) WITH (write_buffer_limit = 2000);
INSERT INTO db SELECT i, 'k' || md5(i::text), md5((i % 4000)::text)::uuid FROM generate_series(6001, 9000) i;
SELECT sift.col_promote('db_idx') > 0 AS promoted;
INSERT INTO db SELECT i, 'k' || md5(i::text), md5(i::text)::uuid FROM generate_series(9001, 12000) i;
SELECT sift.col_promote('db_idx') > 0 AS promoted;
VACUUM (ANALYZE) db;
SELECT count(*) > 1 AS segments, max(format) AS format FROM sift.col_segments('db_idx') WHERE kind = 'immutable';
SELECT sift.col_verify('db_idx', true);

CREATE TABLE db_q (n serial, preds text, heapwhere text);
INSERT INTO db_q (preds, heapwhere)
SELECT format('0:eq:k%s', md5(i::text)), format('s = %L', 'k' || md5(i::text)) FROM unnest(ARRAY[1, 17, 5999, 6001, 6500, 8999, 9001, 11999, 12001, 40000]) i
UNION ALL SELECT format('1:eq:%s', md5(i::text)::uuid), format('u = %L', md5(i::text)::uuid) FROM unnest(ARRAY[1, 17, 3999, 4001, 9005, 11999, 50000]) i
UNION ALL SELECT '0:eq:', 's = ''''' UNION ALL SELECT '0:eq:nothing', 's = ''nothing'''
UNION ALL SELECT format('0:in:k%s|k%s|k%s|none|', md5('1'), md5('7000'), md5('12000')), format('s IN (%L, %L, %L, ''none'', '''')', 'k' || md5('1'), 'k' || md5('7000'), 'k' || md5('12000'))
UNION ALL SELECT format('1:in:%s|%s|%s|%s', md5('1')::uuid, md5('2')::uuid, md5('9005')::uuid, md5('nope')::uuid), format('u IN (%L, %L, %L, %L)', md5('1')::uuid, md5('2')::uuid, md5('9005')::uuid, md5('nope')::uuid)
UNION ALL SELECT format('0:eq:k%s;1:eq:%s', md5('17'), md5('17')::uuid), format('s = %L AND u = %L', 'k' || md5('17'), md5('17')::uuid)
UNION ALL SELECT '0:prefix:kc4', 's LIKE ''kc4%'''
UNION ALL SELECT '0:gt:kf0', 's > ''kf0''' UNION ALL SELECT '0:le:k1f', 's <= ''k1f'''
UNION ALL SELECT format('1:gt:%s', md5('3')::uuid), format('u > %L', md5('3')::uuid);

CREATE FUNCTION db_round(k int) RETURNS TABLE (rounds_ok bool) LANGUAGE sql AS
$$ SELECT count(*) = k * (SELECT count(*) FROM db_q) AND count(*) FILTER (WHERE r = 'ok') = count(*)
   FROM (SELECT db_cmp('db_idx', preds, heapwhere) AS r FROM db_q, generate_series(1, k)) z $$;

SELECT count(*) AS shapes, count(*) FILTER (WHERE (SELECT count(*) FROM sift._test_col_tids('db_idx', preds)) > 0) AS with_rows FROM db_q;
-- 20 rounds in one backend: the first ones search the hash (an equality) or parse the dictionary (the others), the later ones use the parsed dictionary
SELECT db_round(20);

-- written again: the write buffer, deletes and updates, a promotion, a merge, VACUUM, REINDEX
INSERT INTO db SELECT 20000 + i, 'k' || md5((i % 400)::text), md5((i % 300)::text)::uuid FROM generate_series(1, 500) i;
DELETE FROM db WHERE id % 7 = 0 AND id < 12000;
UPDATE db SET s = 'k' || md5((id + 100000)::text) WHERE id % 11 = 0 AND id < 12000;
INSERT INTO db_q (preds, heapwhere) VALUES (format('0:eq:k%s', md5('100011')), format('s = %L', 'k' || md5('100011'))), (format('0:eq:k%s', md5('11')), format('s = %L', 'k' || md5('11')));
SELECT db_round(5);
SELECT sift.col_promote('db_idx') >= 0 AS promoted;
SELECT db_round(5);
SELECT sift.col_merge('db_idx', true) >= 0 AS merged;
VACUUM db;
SELECT db_round(5);
SELECT sift.col_verify('db_idx', true);
REINDEX INDEX db_idx;
SELECT min(format) AS format FROM sift.col_segments('db_idx') WHERE kind = 'immutable';
SELECT sift.col_verify('db_idx', true);
SELECT db_round(5);

-- dict_lookup = off keeps the blocks as before, with no hash, and answers the same
ALTER INDEX db_idx SET (dict_lookup = 'off');
REINDEX INDEX db_idx;
SELECT min(format) AS format FROM sift.col_segments('db_idx') WHERE kind = 'immutable';
SELECT db_round(5);

DROP TABLE db, db_q;
DROP FUNCTION db_cmp(regclass, text, text);
DROP FUNCTION db_round(int);
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
