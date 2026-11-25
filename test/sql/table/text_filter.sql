-- The text tests of a statement (=, <>, ranges, IN, NOT IN, LIKE 'p%') join the zone pruning of a sift_table scan and narrow the rows of each unit on the encoded
-- strings, as in a sift_col index; the rows are those of the normal plan: with NULLs, write buffer rows, tombstones, a column added after the groups were written
-- (a group without it is left to the executor), and a char(n) column.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE tx (id int, g int, s text, k text, c char(6)) USING sift_table;
INSERT INTO tx SELECT i, i % 7, CASE WHEN i % 13 = 0 THEN NULL ELSE 'seg_' || (i % 40) END, md5((i % 5000)::text), 'c' || (i % 9) FROM generate_series(1, 60000) i;
VACUUM (FREEZE, ANALYZE) tx;
\ir test/agg_rows_helpers.sql

CREATE TABLE tx_q (q text);
INSERT INTO tx_q VALUES
	('SELECT g, count(*) FROM tx WHERE s = ''seg_17'' GROUP BY 1'),
	('SELECT g, count(*) FROM tx WHERE s <> ''seg_17'' GROUP BY 1'),
	('SELECT g, count(*) FROM tx WHERE s IN (''seg_1'', ''seg_5'', ''seg_none'') GROUP BY 1'),
	('SELECT g, count(*) FROM tx WHERE s NOT IN (''seg_1'', ''seg_5'') GROUP BY 1'),
	('SELECT g, count(*) FROM tx WHERE s >= ''seg_20'' AND s < ''seg_3'' GROUP BY 1'),
	('SELECT g, count(*) FROM tx WHERE s LIKE ''seg_1%'' GROUP BY 1'),
	('SELECT count(*) FROM tx WHERE k LIKE ''ab%'''),
	('SELECT count(*) FROM tx WHERE k = md5(''7'')'),
	('SELECT count(*) FROM tx WHERE s = ''seg_3'' AND k > ''8'''),
	('SELECT g, count(*) FROM tx WHERE c = ''c4'' GROUP BY 1'),
	('SELECT id, s FROM tx WHERE s = ''seg_9'' ORDER BY s, id DESC LIMIT 5');
SELECT agg_pushed(q) AS pushed, agg_rows(q, true) IS NOT DISTINCT FROM agg_rows(q, false) AS same FROM tx_q ORDER BY q;

INSERT INTO tx SELECT 60000 + i, i % 7, 'seg_' || (i % 40 + 20), md5((i % 300)::text), 'c' || (i % 9) FROM generate_series(1, 3000) i;
DELETE FROM tx WHERE id BETWEEN 500 AND 900;
UPDATE tx SET s = 'seg_17' WHERE id BETWEEN 2000 AND 2030;
SELECT agg_rows(q, true) IS NOT DISTINCT FROM agg_rows(q, false) AS same FROM tx_q ORDER BY q;

ALTER TABLE tx ADD COLUMN w text;
ALTER TABLE tx ADD COLUMN u text DEFAULT 'none';
INSERT INTO tx SELECT 70000 + i, i % 7, 'seg_' || (i % 40), md5(i::text), 'c1', 'w' || (i % 10), 'u' || (i % 4) FROM generate_series(1, 20000) i;
VACUUM tx;
DELETE FROM tx_q;
INSERT INTO tx_q VALUES
	('SELECT g, count(*) FROM tx WHERE w = ''w3'' GROUP BY 1'),
	('SELECT g, count(*) FROM tx WHERE w IS NULL AND s = ''seg_2'' GROUP BY 1'),
	('SELECT g, count(*) FROM tx WHERE u = ''none'' GROUP BY 1'),
	('SELECT g, count(*) FROM tx WHERE u IN (''u1'', ''none'') AND s LIKE ''seg_3%'' GROUP BY 1'),
	('SELECT count(*) FROM tx WHERE w > ''w5'''),
	('SELECT g, count(*) FROM tx WHERE s = ''seg_7'' GROUP BY 1');
SELECT agg_rows(q, true) IS NOT DISTINCT FROM agg_rows(q, false) AS same FROM tx_q ORDER BY q;

DROP TABLE tx_q, tx;
DROP FUNCTION agg_pushed(text), agg_rows(text, bool);
DROP EXTENSION pg_sift;
