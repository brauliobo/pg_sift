-- Inserts never fail for lack of directory entries: with the asynchronous worker off (as when it
-- lags behind), the inserter that finds the directory nearly full merges segments itself.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;
SET enable_seqscan = off;
SET enable_indexscan = off;

CREATE TABLE bp (id serial, body text);
CREATE INDEX bp_idx ON bp USING sift (body) WITH (mutable_limit = 64);

-- every doc has enough distinct terms to fill a 64 kB mutable segment
DO $$
BEGIN
	FOR i IN 1..220 LOOP
		INSERT INTO bp (body) SELECT 'common ' || string_agg('t' || i || 'x' || g, ' ') FROM generate_series(1, 6000) g;
	END LOOP;
END $$;

SELECT count(*) < 120 AS directory_has_room FROM sift.segments('bp_idx');
SELECT count(*) FROM bp WHERE body @@> 'common';
SELECT count(*) FROM bp WHERE body @@> 't7x5999';
SELECT sift.promote('bp_idx') >= 0 AS promoted;
SELECT sift.verify('bp_idx', true);

DROP TABLE bp;
DROP EXTENSION pg_sift;
