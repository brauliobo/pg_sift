-- The merge of segments streams them (a term at a time through the docs of the superblocks it touches, the postings of a large
-- term out of temporary files): at a budget where the docs of two superblocks do not fit and every term of 40 postings is
-- staged, the segment it writes is the one a build of the live rows writes, with and without positions, from segments with dead
-- documents.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET sift.async_maintenance = off;
SET sift.grouped_min_df = 16;
SET max_parallel_maintenance_workers = 0;

-- 35 documents a page: a term runs through 3 superblocks. 'hot' is in nine of ten documents
CREATE TABLE m (id serial, pad text, body text) WITH (autovacuum_enabled = off);
INSERT INTO m (pad, body)
SELECT repeat('x', 100),
       CASE WHEN i % 10 < 9 THEN 'hot ' ELSE '' END ||
       (SELECT string_agg('t' || ((i * 7 + j * 13) % 1009) || ' u' || (i % 31) || CASE WHEN j % 3 = 0 THEN ' t' || i % 5 || ' t' || i % 5 ELSE '' END, ' ')
        FROM generate_series(1, 6) j)
FROM generate_series(1, 20000) i;

-- segments of all the documents, a fourth of them then deleted (the segments keep them, dead, until a merge)
CREATE INDEX m_pos ON m USING sift (body) WITH (build_segments = 4, vacuum_segments = 128);
CREATE INDEX m_nopos ON m USING sift (body) WITH (build_segments = 4, vacuum_segments = 128, positions = off);
DELETE FROM m WHERE id % 4 = 0;
VACUUM m;
SELECT count(*) AS segments, sum(dead_docs) AS dead FROM sift.segments('m_pos') WHERE kind = 'immutable';

SET maintenance_work_mem = '64kB';
SET sift.test_segment_postings = 80;
SELECT sift.merge('m_pos', true);
SELECT sift.merge('m_nopos', true);
RESET sift.test_segment_postings;
RESET maintenance_work_mem;

-- the reference: the live documents in one segment, with positions and without
CREATE INDEX r_pos ON m USING sift (body);
CREATE INDEX r_nopos ON m USING sift (body) WITH (positions = off);

-- one segment without dead documents, of the sections of the reference's
SELECT m.pos, count(*) AS segments, sum(m.dead_docs) AS dead,
       (m.ndocs, m.sum_dl, m.nterms, m.avgdl_ref, m.bytes, m.docmap_bytes, m.postings_bytes, m.dict_bytes) =
       (r.ndocs, r.sum_dl, r.nterms, r.avgdl_ref, r.bytes, r.docmap_bytes, r.postings_bytes, r.dict_bytes) AS same_sections
FROM (SELECT true AS pos, s.* FROM sift.segments('m_pos') s UNION ALL SELECT false, s.* FROM sift.segments('m_nopos') s) m,
     (SELECT true AS pos, s.* FROM sift.segments('r_pos') s UNION ALL SELECT false, s.* FROM sift.segments('r_nopos') s) r
WHERE m.kind = 'immutable' AND r.kind = 'immutable' AND m.pos = r.pos
GROUP BY m.pos, m.ndocs, m.sum_dl, m.nterms, m.avgdl_ref, m.bytes, m.docmap_bytes, m.postings_bytes, m.dict_bytes,
         r.ndocs, r.sum_dl, r.nterms, r.avgdl_ref, r.bytes, r.docmap_bytes, r.postings_bytes, r.dict_bytes
ORDER BY m.pos DESC;

-- every posting (term, tid, tf, dl) is the reference's, and the index verifies
CREATE FUNCTION postings(idx regclass) RETURNS text AS $$
	SELECT md5(string_agg(term || tid::text || tf || ':' || dl, ',' ORDER BY term, tid)) FROM sift._test_postings(idx)
$$ LANGUAGE sql;
SELECT postings('m_pos') = postings('r_pos') AS same_postings_pos, postings('m_nopos') = postings('r_nopos') AS same_postings_nopos;
SELECT sift.verify('m_pos', true), sift.verify('m_nopos', true);

-- a segment of dead documents rewritten alone, with a term above the batch a group holds (its live postings are counted first)
CREATE TABLE h (id serial, body text) WITH (autovacuum_enabled = off);
INSERT INTO h (body) SELECT 'hot' || CASE WHEN i % 3 = 0 THEN ' t' || i % 7 ELSE '' END FROM generate_series(1, 140000) i;
CREATE INDEX h_idx ON h USING sift (body) WITH (vacuum_segments = 128);
DELETE FROM h WHERE id % 5 = 0;
VACUUM h;
SELECT count(*) AS segments, sum(dead_docs) AS dead FROM sift.segments('h_idx') WHERE kind = 'immutable';
SELECT sift.merge('h_idx', true);
CREATE INDEX hr_idx ON h USING sift (body);
SELECT (m.ndocs, m.nterms, m.bytes, m.postings_bytes) = (r.ndocs, r.nterms, r.bytes, r.postings_bytes) AS same_sections,
       m.dead_docs, postings('h_idx') = postings('hr_idx') AS same_postings
FROM sift.segments('h_idx') m, sift.segments('hr_idx') r WHERE m.kind = 'immutable' AND r.kind = 'immutable';
SELECT sift.verify('h_idx', true);
