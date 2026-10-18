-- merge_fanin: segments of a tier that trigger a merge (default 8, 2 to 32), read at merge time: ALTER INDEX, no REINDEX.
CREATE EXTENSION pg_sift;
SET enable_seqscan = off;
CREATE TABLE mf (id serial, body text) WITH (autovacuum_enabled = off);
INSERT INTO mf (body) SELECT 'k' || (i % 7) || ' g' || (i % 1000) || ' common' FROM generate_series(1, 6000) i;

-- range
CREATE INDEX mf_idx ON mf USING sift (body) WITH (build_segments = 24);
SELECT reloptions FROM pg_class WHERE relname = 'mf_idx';
ALTER INDEX mf_idx SET (merge_fanin = 1);
ALTER INDEX mf_idx SET (merge_fanin = 33);
ALTER INDEX mf_idx SET (merge_fanin = 2);
ALTER INDEX mf_idx SET (merge_fanin = 32);
ALTER INDEX mf_idx RESET (merge_fanin);

-- the default (8) merges 24 segments of a tier in two steps; 32 waits for 32 of a tier; 2 merges down to one
SELECT count(*) AS built FROM sift.segments('mf_idx');
SELECT sift.merge('mf_idx') AS merges_default;
SELECT count(*) AS segments FROM sift.segments('mf_idx');
DROP INDEX mf_idx;
CREATE INDEX mf_idx ON mf USING sift (body) WITH (build_segments = 24, merge_fanin = 32);
SELECT sift.merge('mf_idx') AS merges_32;
SELECT count(*) AS segments FROM sift.segments('mf_idx');
ALTER INDEX mf_idx SET (merge_fanin = 2);
SELECT sift.merge('mf_idx') AS merges_2;
SELECT count(*) AS segments FROM sift.segments('mf_idx');
SELECT count(*) AS hits FROM mf WHERE body @@> 'common';
SELECT sift.verify('mf_idx', true);
DROP INDEX mf_idx;

-- every fanin in range leaves the index within the policy's bounds: fewer than fanin segments (all of one tier here), and never
-- more directory entries than the policy tolerates, greatest(32, 2 * fanin), which is below the 112 where inserters make room
CREATE TEMP TABLE sweep (fanin int, built int, merges int, segments int, hits bigint);
DO $$
DECLARE f int; m int; s int; b int; h bigint;
BEGIN
	FOR f IN 2..32 LOOP
		EXECUTE format('CREATE INDEX mf_sweep ON mf USING sift (body) WITH (build_segments = 64, merge_fanin = %s)', f);
		SELECT count(*) INTO b FROM sift.segments('mf_sweep');
		m := sift.merge('mf_sweep');
		SELECT count(*) INTO s FROM sift.segments('mf_sweep');
		SELECT count(*) INTO h FROM mf WHERE body @@> 'common';
		PERFORM sift.verify('mf_sweep', true);
		INSERT INTO sweep VALUES (f, b, m, s, h);
		DROP INDEX mf_sweep;
	END LOOP;
END $$;
SELECT count(*) AS fanins, bool_and(segments < fanin) AS settled, bool_and(segments <= greatest(32, 2 * fanin)) AS bounded,
	bool_and(segments < 112) AS room, bool_and(hits = 6000) AS complete, min(built) AS built_min FROM sweep;
SELECT fanin, built, merges, segments FROM sweep WHERE fanin IN (2, 3, 8, 16, 17, 31, 32) ORDER BY fanin;
DROP TABLE mf;
DROP EXTENSION pg_sift;
