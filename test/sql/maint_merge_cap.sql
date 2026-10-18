-- One merge reads at most sift.test_merge_max_kb of segments (16 GB): its output stays within what a
-- segment header can list, whatever sift.merge(index, true) was asked. Segments past the cap stay.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET sift.async_maintenance = off;
SET enable_seqscan = off;
CREATE TABLE mc (id serial, body text) WITH (autovacuum_enabled = off);
CREATE INDEX mc_idx ON mc USING sift (body);

-- 10 segments of about 112 kB
DO $$
BEGIN
	FOR i IN 1..10 LOOP
		INSERT INTO mc (body) SELECT 'common g' || i || ' ' || string_agg(md5((i * 1000 + g * 7 + j)::text), ' ')
			FROM generate_series(1, 400) g, generate_series(1, 6) j GROUP BY g;
		PERFORM sift.promote('mc_idx');
		COMMIT;
	END LOOP;
END $$;
SELECT count(*) AS segments, min(bytes) BETWEEN 100000 AND 130000 AS about_112kb FROM sift.segments('mc_idx') WHERE kind = 'immutable';

SET sift.test_merge_max_kb = 500;
SELECT sift.merge('mc_idx', true) >= 1 AS merged;
SELECT sift.merge('mc_idx', true) >= 0 AS merged_more;
SELECT count(*) > 1 AS in_pieces, max(bytes) <= 500 * 1024 AS within_cap FROM sift.segments('mc_idx') WHERE kind = 'immutable';
SELECT count(*) FROM mc WHERE body @@> 'common';
SELECT count(*) FROM mc WHERE body @@> 'g7';
SELECT sift.verify('mc_idx', true);

RESET sift.test_merge_max_kb;
SELECT sift.merge('mc_idx', true) >= 1 AS merged;
SELECT count(*) AS segments FROM sift.segments('mc_idx') WHERE kind = 'immutable';
SELECT count(*) FROM mc WHERE body @@> 'common';
SELECT sift.verify('mc_idx', true);

DROP TABLE mc;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
