-- The VACUUM of a sift_table that has an index joins the small parts that a compaction under a small maintenance budget wrote one granule each: the granules are copied as they are, the address hash
-- directory of the joined part is built again from the addresses of its rows, and the rows stay where the index finds them. Every check prints t.
-- requires: build col
CREATE EXTENSION IF NOT EXISTS pg_sift;
-- a testing build creates Format 1 relations unless told otherwise; a release build has no such setting
DO $$ BEGIN PERFORM set_config('sift.test_create_format1', 'off', false); EXCEPTION WHEN OTHERS THEN NULL; END $$;
SET maintenance_work_mem = '4MB';

CREATE TABLE jp (id int PRIMARY KEY, grp int NOT NULL, v bigint NOT NULL, pad text NOT NULL) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO jp SELECT g, g % 64, g * 3, repeat('x', 40) FROM generate_series(1, 32768) g;
VACUUM jp;

-- a quarter of the rows deleted and as many inserted, four times, with a VACUUM after each
\o /dev/null
SELECT stmt FROM generate_series(1, 4) c,
	LATERAL unnest(ARRAY[
		format('DELETE FROM jp WHERE id > %s AND id <= %s', (c - 1) * 8192, c * 8192),
		format('INSERT INTO jp SELECT 32768 + %s + g, g %% 64, g * 3, repeat(''x'', 40) FROM generate_series(1, 8192) g', (c - 1) * 8192),
		'VACUUM jp']) WITH ORDINALITY AS u(stmt, ord) ORDER BY c, ord
\gexec
\o

SELECT count(*) = 32768 AND count(DISTINCT id) = 32768 AS every_row_once FROM jp;
SELECT count(*) <= 2 AS at_most_two_parts, sum(rows) = 32768 AS parts_hold_the_rows, sum(dead_rows) = 0 AS no_dead_rows FROM sift.parts('jp');
SELECT count(*) = 0 AS check_finds_nothing FROM sift.check('jp', true);

-- the address of a row is found through the index in every part, the joined ones included
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*) = 1000 AS index_fetches_by_address FROM generate_series(1, 1000) i, LATERAL (SELECT id FROM jp WHERE id = 32768 + (i * 32) % 32768 + 1) f;
SELECT count(*) = 32768 AS index_scan_reads_every_row FROM jp WHERE id >= 8193;

DROP TABLE jp;
DROP EXTENSION pg_sift;
