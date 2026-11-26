-- The step that packs tiny parts swaps the descriptors of the parts of one partition in one commit of the manifest, not in one for each part: sixteen tiny parts of level 1 take at most two.
-- requires: build col
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;

CREATE TABLE pk (v int);
CREATE INDEX pk_idx ON pk USING sift_store_test (v);
SELECT sift._test_f2_init('pk_idx') IS NOT NULL AS init;
SELECT count(sift._test_f2_add_part('pk_idx', 1, NULL, 8, 1000 + 100 * g, g * 100, 0, 1)) AS parts FROM generate_series(1, 16) g;
SELECT count(*) AS pages, bool_and(inline) AS inline
FROM (SELECT DISTINCT i.header_blk, i.inline FROM sift._test_f2_partitions('pk_idx') p, LATERAL sift._test_f2_pack_info('pk_idx', p.part_id, p.key_lo) i) q;
SELECT manifest_version AS before FROM sift._test_f2_meta('pk_idx') \gset

SELECT sift._test_f2_pack_tails('pk_idx') AS packed;
SELECT manifest_version - :before <= 2 AS commits_at_most_two FROM sift._test_f2_meta('pk_idx');
SELECT count(DISTINCT i.header_blk) AS pages, bool_and(i.packed) AS packed
FROM sift._test_f2_partitions('pk_idx') p, LATERAL sift._test_f2_pack_info('pk_idx', p.part_id, p.key_lo) i;
SELECT sift._test_f2_check('pk_idx') ~ '^parts=16 ' AS consistent;
SELECT sift._test_f2_pack_tails('pk_idx') AS packed_again;

DROP TABLE pk;
RESET client_min_messages;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
