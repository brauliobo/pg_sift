-- Dropping, rebuilding or altering one index that shares a value store (docs/COL-VSTORE.md) never touches what the other members read: after each step every
-- shape answers as the heap does, the store still has the values it had, and it goes with the last member and not before. The steps are rows of a table.
SET client_min_messages = warning;
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET sift.async_maintenance = off;
SET sift.test_vstore_columns = 'uid,url';

CREATE TABLE vd (id int PRIMARY KEY, tenant int, uid uuid, url text);
INSERT INTO vd SELECT i, i % 7, md5((i % 2500)::text)::uuid,
	CASE WHEN i % 29 = 0 THEN NULL ELSE 'https://example.com/p/' || md5((i % 3000)::text) END FROM generate_series(1, 6000) i;
ANALYZE vd;
CREATE INDEX vd_a ON vd USING sift_col (id, tenant, uid, url);
CREATE INDEX vd_b ON vd USING sift_col (id, uid, url);
CREATE INDEX vd_c ON vd USING sift_col (uid, url, tenant);

CREATE TABLE vd_q (n serial, q text);
INSERT INTO vd_q (q) VALUES
 ('SELECT count(*), sum(id) FROM vd WHERE url = ''https://example.com/p/'' || md5(''77'')'),
 ('SELECT count(*), sum(id) FROM vd WHERE url IN (''https://example.com/p/'' || md5(''1''), ''https://example.com/p/'' || md5(''2999''), '''', ''none'')'),
 ('SELECT count(*), sum(id) FROM vd WHERE url > ''https://example.com/p/5'' AND url < ''https://example.com/p/6'''),
 ('SELECT count(*), sum(id) FROM vd WHERE url IS NULL'),
 ('SELECT count(*), sum(id) FROM vd WHERE uid = md5(''5'')::uuid'),
 ('SELECT count(*), sum(id) FROM vd WHERE uid IN (md5(''1'')::uuid, md5(''2499'')::uuid, md5(''x'')::uuid)'),
 ('SELECT count(*), count(DISTINCT url), min(url), max(url), count(DISTINCT uid) FROM vd'),
 ('SELECT md5(string_agg(g::text, '','' ORDER BY g)) FROM (SELECT count(*) || ''/'' || url AS g FROM vd GROUP BY url) z'),
 ('SELECT md5(string_agg(g::text, '','' ORDER BY g)) FROM (SELECT count(*) || ''/'' || uid AS g FROM vd WHERE tenant = 2 GROUP BY uid) z');

-- every shape on the indexes and on the heap
CREATE FUNCTION vd_check(label text) RETURNS TABLE (step text, shapes int, same int) LANGUAGE plpgsql AS $$
DECLARE r record; a text; b text; cnt int := 0; ok int := 0;
BEGIN
	FOR r IN SELECT q FROM vd_q ORDER BY vd_q.n LOOP
		SET LOCAL sift.enable_agg = on;
		EXECUTE r.q INTO a;
		SET LOCAL sift.enable_agg = off;
		EXECUTE r.q INTO b;
		cnt := cnt + 1; ok := ok + (a IS NOT DISTINCT FROM b)::int;
	END LOOP;
	RETURN QUERY SELECT label, cnt, ok;
END $$;

-- the values of a store by code: one fingerprint of them all
CREATE FUNCTION vd_values(store regclass) RETURNS text LANGUAGE sql AS $$
	SELECT count(*) || ':' || md5(string_agg(sift._test_vstore_value(store, c), ',' ORDER BY c))
	  FROM generate_series(0, (SELECT view_values FROM sift._test_vstore_info(store)) - 1) c $$;

CREATE TABLE vd_base AS SELECT vd_values('vd_uid_sift_values') AS uid, vd_values('vd_url_sift_values') AS url;

-- what a step leaves: the stores and who uses them, whether the values are the ones of the start, and what vd_c does with its strings
CREATE FUNCTION vd_state(label text) RETURNS TABLE (step text, stores text, values_same boolean, vd_c text) LANGUAGE sql AS $$
	SELECT label, coalesce(string_agg(column_name || '=' || indexes_using, ' ' ORDER BY column_name), 'none'),
	       coalesce(bool_and(vd_values(store) = CASE column_name WHEN 'uid' THEN (SELECT uid FROM vd_base) ELSE (SELECT url FROM vd_base) END), true),
	       (SELECT coalesce(string_agg(DISTINCT state, ','), '-') FROM sift.col_value_store_members() WHERE index_name::text = 'vd_c')
	  FROM sift.col_value_store_status WHERE table_name = 'vd'::regclass $$;

SELECT * FROM vd_check('built'), vd_state('built');

CREATE TABLE vd_steps (n int, label text, sql text);
INSERT INTO vd_steps VALUES
 (10, 'drop the first member (its build made the store)',  'DROP INDEX vd_a'),
 (20, 'a member made after it takes the store again',      'CREATE INDEX vd_d ON vd USING sift_col (id, uid, url)'),
 (30, 'drop a middle member concurrently',                 'DROP INDEX CONCURRENTLY vd_b'),
 (40, 'reindex a member',                                  'REINDEX INDEX vd_c'),
 (50, 'reindex a member concurrently',                     'REINDEX INDEX CONCURRENTLY vd_d'),
 (55, 'only the rule shares from here',                    'RESET sift.test_vstore_columns'),
 (60, 'its order option changes',                          'ALTER INDEX vd_c SET (key_order = ''tenant'')'),
 (70, 'reindex: it has another order, its own strings',    'REINDEX INDEX vd_c'),
 (80, 'the order is the same again',                       'ALTER INDEX vd_c RESET (key_order)'),
 (90, 'reindex: it shares again',                          'REINDEX INDEX vd_c'),
 (95, 'every string column shares again',                  'SET sift.test_vstore_columns = ''uid,url'''),
 (100, 'vacuum full of the table',                         'VACUUM FULL vd'),
 (110, 'cluster of the table',                             'CLUSTER vd USING vd_pkey'),
 (120, 'vacuum full of a store',                           'VACUUM FULL vd_url_sift_values'),
 (130, 'vacuum of the table',                              'VACUUM vd'),
 (140, 'a column type changes',                            'ALTER TABLE vd ALTER COLUMN url TYPE varchar(80)'),
 (150, 'drop a member: the last but one',                  'DROP INDEX vd_c'),
 (160, 'drop the last member: the stores go with it',      'DROP INDEX vd_d'),
 (170, 'a build after it makes new stores',                'CREATE INDEX vd_e ON vd USING sift_col (id, uid, url)'),
 (180, 'a second one shares them',                         'CREATE INDEX vd_f ON vd USING sift_col (uid, id, url)'),
 (190, 'drop one of two members',                         'DROP INDEX vd_e');

SELECT unnest(ARRAY[sql, format('SELECT * FROM vd_check(%L), vd_state(%L)', label, label)]) FROM vd_steps ORDER BY n \gexec

-- nobody can drop a store under a member, by its name or by the table
DROP TABLE vd_uid_sift_values;
DROP TABLE vd_url_sift_values;
SELECT * FROM vd_check('after the refused drops');

-- a column that goes drops the indexes that hold it, and the stores that only they used; a member without it stays whole
CREATE INDEX vd_g ON vd USING sift_col (id, uid);
SELECT * FROM vd_state('a member of the uid only');
ALTER TABLE vd DROP COLUMN url;
SELECT * FROM vd_state('the url column dropped');
SELECT count(*), count(DISTINCT uid), sum(id) FROM vd WHERE uid = md5('5')::uuid;
SET sift.enable_agg = off;
SELECT count(*), count(DISTINCT uid), sum(id) FROM vd WHERE uid = md5('5')::uuid;
RESET sift.enable_agg;
SELECT sift.col_verify('vd_g', true);
DROP INDEX vd_g;
SELECT * FROM vd_state('the last member of the last store dropped');

-- an owner without rights on the sift schema drops the last member and the stores go with it
CREATE ROLE vd_owner;
CREATE SCHEMA vd_s AUTHORIZATION vd_owner;
CREATE TABLE vd_s.r (id int, uid uuid, url text);
ALTER TABLE vd_s.r OWNER TO vd_owner;
INSERT INTO vd_s.r SELECT i, md5((i % 50)::text)::uuid, 'https://example.com/' || md5((i % 90)::text) FROM generate_series(1, 3000) i;
ANALYZE vd_s.r;
SET ROLE vd_owner;
CREATE INDEX r1 ON vd_s.r USING sift_col (id, uid, url);
CREATE INDEX r2 ON vd_s.r USING sift_col (uid, id, url);
RESET ROLE;
SELECT count(*) FROM sift.col_value_store_status WHERE table_name = 'vd_s.r'::regclass;
SET ROLE vd_owner;
DROP INDEX vd_s.r1;
DROP INDEX vd_s.r2;
RESET ROLE;
SELECT count(*) FROM sift.col_value_store_status WHERE table_name = 'vd_s.r'::regclass;
DROP SCHEMA vd_s CASCADE;
DROP ROLE vd_owner;

DROP TABLE vd_q, vd_steps, vd_base;
DROP FUNCTION vd_check(text), vd_state(text), vd_values(regclass);
DROP TABLE vd;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
