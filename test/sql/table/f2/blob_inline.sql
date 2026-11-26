-- A wide value of a sift_table (over 4096 bytes) that compresses to no more than 4096 bytes is kept in its reference, in a page of no extent; one that does not compress that far keeps pages of its
-- own. Both read back, change, go and are reclaimed the same: the table is as small as its compressed values, and the values are what they were after updates, deletes, VACUUM and a merge.
-- requires: build col
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;

CREATE FUNCTION bi_val(i int) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT CASE i % 3
		WHEN 0 THEN (SELECT string_agg(md5((i * 1000 + j)::text), '') FROM generate_series(1, 70 + i % 20) j) || (SELECT string_agg(md5((i * 1000 + j)::text), '') FROM generate_series(1, 70 + i % 20) j)
		WHEN 1 THEN (SELECT string_agg(md5((i * 1000 + j)::text), '') FROM generate_series(1, 150) j)
		ELSE repeat('z' || i, 30000) END $$;
CREATE TABLE bi (id int, v text) USING sift_table;
CREATE TABLE bi_ref AS SELECT i AS id, bi_val(i) AS v FROM generate_series(1, 600) i;
INSERT INTO bi SELECT id, v FROM bi_ref;
SELECT count(*) AS rows, bool_and(b.v = r.v) AS same FROM bi b JOIN bi_ref r USING (id);

-- only compressible values: no page of their own, so the file is a fraction of a page for each row
CREATE TABLE bc (id int, v text) USING sift_table;
INSERT INTO bc SELECT id, v FROM bi_ref WHERE id % 3 = 0;
SELECT (SELECT relation_blocks FROM sift.space('bc')) < (SELECT count(*) FROM bc) / 2 AS no_page_for_each_value;

UPDATE bi SET v = bi_val(id + 1000) WHERE id % 7 = 0;
UPDATE bi_ref SET v = bi_val(id + 1000) WHERE id % 7 = 0;
DELETE FROM bi WHERE id % 5 = 0;
DELETE FROM bi_ref WHERE id % 5 = 0;
SELECT count(*) AS rows, bool_and(b.v = r.v) AS same FROM bi b JOIN bi_ref r USING (id);
VACUUM bi;
SELECT busy, dead_rows FROM sift.maintain('bi');
SELECT count(*) AS rows, bool_and(b.v = r.v) AS same FROM bi b JOIN bi_ref r USING (id);
SELECT count(*) AS findings FROM sift.check('bi', false, 100);

DROP TABLE bi, bi_ref, bc;
DROP FUNCTION bi_val(int);
RESET client_min_messages;
DROP EXTENSION pg_sift;
