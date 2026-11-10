-- The header page of a segment keeps a copy (the mirror) of the small ranges of its stream that a plan and the first read of the segment open: the
-- schema, the unit directory, the point directories, the order headers, the headers of the pre-aggregates. A read of a range takes the copy in place
-- of the pages of the stream, sift.col_verify checks every range against the stream, and the damage of the copy or of the stream under it is
-- refused. A segment without one (mirror_bytes = 0, the segments of older releases) is read from its stream with the same answers.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;

CREATE TABLE mr (id int, grp int, label text, amount int) WITH (autovacuum_enabled = off);
INSERT INTO mr SELECT g, g % 50, 'l' || g % 90, g % 100 FROM generate_series(1, 30000) g;
CREATE INDEX mr_idx ON mr USING sift_col (id, grp, label, amount) WITH (write_buffer_limit = 4096);
SET sift.test_limits = 'mirror_bytes=0';
CREATE INDEX mr_plain ON mr USING sift_col (id, grp, label, amount) WITH (write_buffer_limit = 4096);
RESET sift.test_limits;

-- the header page of the segment is longer by the mirror, and both are sound
SELECT header AS mr_hdr FROM sift._test_segments('mr_idx') \gset
SELECT header AS pl_hdr FROM sift._test_segments('mr_plain') \gset
SELECT (SELECT lower FROM sift._test_page('mr_idx', :mr_hdr)) > (SELECT lower FROM sift._test_page('mr_plain', :pl_hdr)) AS mirror_in_header;
SELECT (SELECT lower FROM sift._test_page('mr_plain', :pl_hdr)) AS plain_lower \gset
SELECT sift.col_verify('mr_idx'::regclass);
SELECT sift.col_verify('mr_plain'::regclass);

-- the answers of a scan do not depend on it
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT count(*), sum(amount), min(label), max(id) FROM mr WHERE grp = 7;
SELECT count(*) > 0 AS reset FROM sift._test_col_dict_cache(true);
SELECT count(*), sum(amount), min(label), max(id) FROM mr WHERE grp = 7;
RESET enable_seqscan; RESET enable_bitmapscan;

-- the segment of a merge has its mirror
CREATE TABLE mm (id int, grp int, label text, amount int) WITH (autovacuum_enabled = off);
CREATE INDEX mm_idx ON mm USING sift_col (id, grp, label, amount) WITH (write_buffer_limit = 4096);
INSERT INTO mm SELECT g, g % 50, 'l' || g % 90, g % 100 FROM generate_series(1, 6000) g;
SELECT sift.col_promote('mm_idx'::regclass) IS NOT NULL AS promoted_a;
INSERT INTO mm SELECT g, g % 50, 'l' || g % 90, g % 100 FROM generate_series(6001, 12000) g;
SELECT sift.col_promote('mm_idx'::regclass) IS NOT NULL AS promoted_b;
SELECT count(*) > 1 AS two_segments FROM sift.col_segments('mm_idx'::regclass);
SELECT sift.col_merge('mm_idx'::regclass, true) > 0 AS merged;
SELECT count(*) AS one_segment FROM sift.col_segments('mm_idx'::regclass) WHERE kind = 'immutable';
SELECT b AS mm_hdr FROM generate_series(0, pg_relation_size('mm_idx') / 8192 - 1) b,
       LATERAL sift._test_page('mm_idx', b) p, sift.col_segments('mm_idx'::regclass) s
WHERE p.type = 3 AND p.gen = s.gen AND s.kind = 'immutable' \gset
SELECT (SELECT lower FROM sift._test_page('mm_idx', :mm_hdr)) > :plain_lower AS merged_keeps_mirror;
SELECT sift.col_verify('mm_idx'::regclass);

-- the damage of the mirror is refused when the segment is opened or checked: its sum, the head of it, one range of it
\set mirror_end :plain_lower
SELECT sift._test_poke('mr_idx', :mr_hdr, :mirror_end + 40, 255, 1);
SELECT count(*) > 0 AS reset FROM sift._test_col_dict_cache(true);
SELECT sift.col_verify('mr_idx'::regclass);
REINDEX INDEX mr_idx;
SELECT header AS mr_hdr FROM sift._test_segments('mr_idx') \gset
SELECT sift._test_poke('mr_idx', :mr_hdr, :mirror_end + 8, 255, 1);
SELECT sift.col_verify('mr_idx'::regclass);
REINDEX INDEX mr_idx;
SELECT header AS mr_hdr FROM sift._test_segments('mr_idx') \gset
SELECT sift._test_poke('mr_idx', :mr_hdr, :mirror_end + 16, 4000000000, 4);
SELECT sift.col_verify('mr_idx'::regclass);

-- a mirror that no longer says what the stream says (here the stream under its first range) is refused by the check
REINDEX INDEX mr_idx;
SELECT sift._test_poke('mr_idx', 3, 4132, 255, 1);
SELECT count(*) > 0 AS reset FROM sift._test_col_dict_cache(true);
SELECT sift.col_verify('mr_idx'::regclass);

-- a header page whose mirror has no magic (an older segment) is read from its stream
REINDEX INDEX mr_idx;
SELECT header AS mr_hdr FROM sift._test_segments('mr_idx') \gset
SELECT sift._test_poke('mr_idx', :mr_hdr, :mirror_end, 0, 4);
SELECT count(*) > 0 AS reset FROM sift._test_col_dict_cache(true);
SELECT sift.col_verify('mr_idx'::regclass);
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT count(*), sum(amount), min(label), max(id) FROM mr WHERE grp = 7;
RESET enable_seqscan; RESET enable_bitmapscan;

DROP TABLE mr, mm;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
