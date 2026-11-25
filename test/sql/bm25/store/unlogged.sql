-- Unlogged indexes get an init fork (ambuildempty, INIT target); temp indexes use local buffers
CREATE EXTENSION pg_sift_test CASCADE;
CREATE FUNCTION info(idx regclass) RETURNS jsonb LANGUAGE sql
	AS $$ SELECT jsonb_object_agg(key, value) FROM sift._test_info(idx)
		WHERE key IN ('high_water', 'nblocks', 'init_blocks', 'next_gen', 'nsegs') $$;

CREATE UNLOGGED TABLE u (v int);
INSERT INTO u VALUES (5000), (50000);
CREATE INDEX u_idx ON u USING sift_store_test (v);
SELECT info('u_idx');
SELECT pg_relation_size('u_idx', 'init') AS init_bytes;
SELECT sift._test_seg_write('u_idx', 300000, 3, 0, 64) AS g \gset
SELECT count(*), bool_and(sift._test_seg_read('u_idx', gen) = payload) FROM sift._test_segments('u_idx');
SELECT sift._test_seg_drop('u_idx', :g) > 0, sift._test_check('u_idx');

CREATE TEMP TABLE tmp (v int);
INSERT INTO tmp VALUES (1), (70000);
CREATE INDEX tmp_idx ON tmp USING sift_store_test (v);
SELECT info('tmp_idx');
SELECT sift._test_seg_write('tmp_idx', 2000000, 4) AS g \gset
SELECT count(*), bool_and(sift._test_seg_read('tmp_idx', gen) = payload),
	bool_and(sift._test_seg_read('tmp_idx', gen, 3) = size) FROM sift._test_segments('tmp_idx');
SELECT sift._test_seg_drop('tmp_idx', :g) > 0, sift._test_check('tmp_idx');

DROP TABLE u, tmp;
DROP FUNCTION info(regclass);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
