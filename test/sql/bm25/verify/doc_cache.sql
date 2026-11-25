-- sift.verify and sift.segments look the dl of the docs of a posting up in the superblocks the term touches, kept within maintenance_work_mem / 2 as
-- the merge keeps them, and read the position records through a window: they need no array that follows the docs of the segment or the postings of
-- a term. At the smallest budget a superblock does not stay for the next term, and the answers are those of the default.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;

-- 40000 docs over several superblocks (the pad), two terms in every doc and positions; some docs are vacuumed
CREATE TABLE vc (id serial PRIMARY KEY, body text, pad text) WITH (autovacuum_enabled = off);
INSERT INTO vc (body, pad) SELECT 'common w' || (i % 7) || ' x' || (i % 1009) || ' common ' || CASE WHEN i % 3 = 0 THEN 'common' ELSE 'rare' || i END, repeat('p', 100)
	FROM generate_series(1, 40000) i;
CREATE INDEX vc_idx ON vc USING sift (body);
DELETE FROM vc WHERE id % 11 = 0;
VACUUM vc;
SELECT count(*) AS segments, bool_or(sb_hi > sb_lo + 1) AS several_superblocks, bool_or(dirty) AS dirty FROM sift.segments('vc_idx');

CREATE TABLE vc_default AS SELECT * FROM sift.segments('vc_idx');
SELECT sift.verify('vc_idx', true) AS default_budget;

SET maintenance_work_mem = '64kB';
SELECT sift.verify('vc_idx', true) AS smallest_budget;
CREATE TABLE vc_small AS SELECT * FROM sift.segments('vc_idx');
RESET maintenance_work_mem;
SELECT (SELECT count(*) FROM (SELECT * FROM vc_default EXCEPT SELECT * FROM vc_small) d) AS only_default,
	(SELECT count(*) FROM (SELECT * FROM vc_small EXCEPT SELECT * FROM vc_default) d) AS only_smallest;

DROP TABLE vc, vc_default, vc_small;
DROP EXTENSION pg_sift;
