-- an index built by another analyzer version (or analyzer options) is refused for reads, writes and verify with a
-- REINDEX hint, and a REINDEX makes it usable again
CREATE EXTENSION pg_sift_test CASCADE;
CREATE TABLE av (id serial, body text);
INSERT INTO av (body) SELECT 'alpha beta ' || i FROM generate_series(1, 500) i;
CREATE INDEX av_idx ON av USING sift (body);
SELECT count(*) FROM av WHERE body @@> 'alpha';

-- metapage: u32 analyzer_hash at byte 40 of block 0
SELECT sift._test_poke('av_idx', 0, 40, 1, 1);
SELECT count(*) FROM av WHERE body @@> 'alpha';
SELECT id FROM av WHERE body @@> 'beta' ORDER BY sift.score(ctid) DESC LIMIT 1;
INSERT INTO av (body) VALUES ('gamma');
SELECT sift.verify('av_idx');
SELECT sift.promote('av_idx');
VACUUM av;
DELETE FROM av WHERE id = 1;

REINDEX INDEX av_idx;
SELECT count(*) FROM av WHERE body @@> 'alpha';
INSERT INTO av (body) VALUES ('gamma');
SELECT sift.verify('av_idx', true);
SELECT count(*) FROM av WHERE body @@> 'gamma';

-- the analyzer hashes 1.2.0 stored for this analyzer: without phrase_index = pairs it is the one of this build, with it
-- (reloption removed in 1.3.0) none of this build, so such an index is refused until a REINDEX
SELECT sift._test_poke('av_idx', 0, 40, 2028147818, 4);
SELECT count(*) FROM av WHERE body @@> 'gamma';
SELECT sift._test_poke('av_idx', 0, 40, 1302474567, 4);
SELECT count(*) FROM av WHERE body @@> 'gamma';
SELECT sift.verify('av_idx');
REINDEX INDEX av_idx;
SELECT count(*) FROM av WHERE body @@> 'gamma';

DROP TABLE av;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
