-- A healing merge on a sift_table (S119-02): the docs of a segment without positions are read again in the order of the segment, not
-- through the read-ahead window of a promotion, and the merged segment decides its phrases from the positions as a fresh build does.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;

CREATE TABLE docs (id int, body text) USING sift_table;
CREATE INDEX docs_idx ON docs USING sift (body);
INSERT INTO docs SELECT g, 'alpha beta gamma ' || g FROM generate_series(1, 300) g;
SET sift.test_no_heap_positions = on;
SELECT sift.promote('docs_idx') >= 0 AS promoted_without_positions;
RESET sift.test_no_heap_positions;
INSERT INTO docs SELECT g, 'gamma beta alpha ' || g FROM generate_series(301, 600) g;
SELECT sift.promote('docs_idx') >= 0 AS promoted_with_positions;
SELECT sift.merge('docs_idx', true) >= 0 AS merged;
SELECT count(*) AS segments FROM sift.segments('docs_idx') WHERE kind = 'immutable';

SET enable_seqscan = off;
SELECT count(*) AS alpha_beta FROM docs WHERE body @@> '"alpha beta"';
SELECT count(*) AS beta_alpha FROM docs WHERE body @@> '"beta alpha"';
SELECT sift.verify('docs_idx', true) IS NOT NULL AS verified;

DROP TABLE docs;
DROP EXTENSION pg_sift;
