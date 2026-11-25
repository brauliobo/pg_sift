-- A column added with a default reads that default on the rows of the parts written before it, and the reads go on after the relation cache reloads the descriptor the default came from (a change of
-- the defaults of the table builds a new descriptor and frees the old one, which a server built with assertions overwrites).
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;

CREATE TABLE tickets USING sift_table AS SELECT g AS id, g % 7 AS kind FROM generate_series(1, 300) g;
ALTER TABLE tickets ADD COLUMN label text DEFAULT 'legacy default of a text column that is longer than a word';
ALTER TABLE tickets ADD COLUMN ratio numeric(10, 2) DEFAULT 2.50;
ALTER TABLE tickets ADD COLUMN token uuid DEFAULT '00000000-0000-0000-0000-00000000002a';

SELECT count(label), min(label), max(label), count(DISTINCT label) FROM tickets;
ALTER TABLE tickets ALTER COLUMN label SET DEFAULT 'a default for the rows that follow';
ANALYZE tickets;
SELECT count(*) FILTER (WHERE label LIKE 'legacy default%') AS label_rows, sum(ratio) AS ratio_sum, count(*) FILTER (WHERE token = '00000000-0000-0000-0000-00000000002a') AS token_rows FROM tickets;
ALTER TABLE tickets ALTER COLUMN ratio SET DEFAULT 9.99;
ALTER TABLE tickets ALTER COLUMN token DROP DEFAULT;
SELECT label, ratio, token FROM tickets WHERE id = 299;
INSERT INTO tickets (id, kind) VALUES (301, 1);
SELECT label, ratio, token FROM tickets WHERE id = 301;
SELECT label, count(*) FROM tickets WHERE kind = 1 GROUP BY label ORDER BY 2 DESC, 1 LIMIT 2;
DROP TABLE tickets;
DROP EXTENSION pg_sift;
