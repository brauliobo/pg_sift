-- Index deletion on a sift_table: the b-tree hands the table AM the TIDs of an index page to learn which are dead (simple deletion of
-- entries marked dead, bottom-up deletion before a page splits), and the AM answers from its tombstone log, reading a row only to learn
-- that a simple deletion's row is gone. The rows updated here keep their index keys, so every secondary index fills with versions that
-- only the deletion passes remove; the tables must answer as a heap twin does throughout, and the indexes must stay bounded (the passes
-- find the dead versions).
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET sift.table_inline_promote = on;
CREATE TABLE t (id int, k int, grp int, v int, note text) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE h (id int, k int, grp int, v int, note text) WITH (autovacuum_enabled = false);
INSERT INTO t SELECT g, (g * 7919) % 100000, g % 97, g, 'n' || g FROM generate_series(1, 100000) g;
INSERT INTO h SELECT * FROM t;
ALTER TABLE t ADD PRIMARY KEY (id);
ALTER TABLE h ADD PRIMARY KEY (id);
-- a key that scatters the TIDs of an index page over the superblocks, and one that groups them
CREATE INDEX t_k ON t (k);
CREATE INDEX t_grp ON t (grp);
-- (v is indexed too: the updates below change it, so they are not HOT and every index fills with versions)
CREATE INDEX t_v ON t (v);
CREATE INDEX h_k ON h (k);
CREATE INDEX h_grp ON h (grp);
CREATE INDEX h_v ON h (v);
SELECT sift.table_maintain('t') IS NOT NULL AS maintained;
VACUUM (ANALYZE) t, h;
SELECT (SELECT count(*) FROM sift.table_segments('t') WHERE kind = 'columns') > 0 AS promoted;
CREATE FUNCTION same() RETURNS bool LANGUAGE sql AS
$$ SELECT (SELECT count(*) FROM (SELECT * FROM t EXCEPT ALL SELECT * FROM h) x) = 0 AND (SELECT count(*) FROM (SELECT * FROM h EXCEPT ALL SELECT * FROM t) y) = 0 $$;
CREATE FUNCTION idx_pages(i regclass) RETURNS bigint LANGUAGE sql AS $$ SELECT pg_relation_size(i) / 8192 $$;

-- 20 rounds of updates that keep every index key: the old versions pile up in t_k and t_grp until a deletion pass removes them
DO $$ BEGIN
	FOR r IN 1..20 LOOP
		UPDATE t SET v = v + 1 WHERE id % 50 = r;
		UPDATE h SET v = v + 1 WHERE id % 50 = r;
	END LOOP;
END $$;
SELECT same();
-- the rows found through each index are the heap twin's
SET enable_seqscan = off;
SELECT (SELECT count(*) FROM t WHERE k BETWEEN 1000 AND 2000) = (SELECT count(*) FROM h WHERE k BETWEEN 1000 AND 2000) AS by_k,
       (SELECT sum(v) FROM t WHERE grp = 7) = (SELECT sum(v) FROM h WHERE grp = 7) AS by_grp,
       (SELECT v FROM t WHERE id = 4242) = (SELECT v FROM h WHERE id = 4242) AS by_id;
RESET enable_seqscan;
-- the scattered-key index stays within a few times its size after the rounds (the versions are removed by the passes; a heap's index grows alike)
SELECT idx_pages('t_k') < 4 * idx_pages('h_k') AS t_k_bounded, idx_pages('t_grp') < 4 * idx_pages('h_grp') AS t_grp_bounded;

-- the same with the write buffer holding many rows that interleave with the old ones in the index pages
INSERT INTO t SELECT g + 200000, (g * 7919) % 100000, g % 97, g, 'w' || g FROM generate_series(1, 30000) g;
INSERT INTO h SELECT g + 200000, (g * 7919) % 100000, g % 97, g, 'w' || g FROM generate_series(1, 30000) g;
DO $$ BEGIN
	FOR r IN 21..40 LOOP
		UPDATE t SET v = v + 1 WHERE id % 50 = r;
		UPDATE h SET v = v + 1 WHERE id % 50 = r;
	END LOOP;
END $$;
SELECT same();
SET enable_seqscan = off;
SELECT (SELECT count(*) FROM t WHERE k BETWEEN 1000 AND 2000) = (SELECT count(*) FROM h WHERE k BETWEEN 1000 AND 2000) AS by_k,
       (SELECT sum(v) FROM t WHERE grp = 7) = (SELECT sum(v) FROM h WHERE grp = 7) AS by_grp;
RESET enable_seqscan;
SELECT idx_pages('t_k') < 4 * idx_pages('h_k') AS t_k_bounded, idx_pages('t_grp') < 4 * idx_pages('h_grp') AS t_grp_bounded;
-- a VACUUM after the rounds leaves the indexes answering as before
VACUUM t, h;
SELECT same();
DROP TABLE t, h;
DROP FUNCTION same(), idx_pages(regclass);
DROP EXTENSION pg_sift;
