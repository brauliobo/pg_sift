-- sift.col_sections: where the bytes of a sift_col index and of a sift_table go. The rows add up to the size of the relation, every leaf has
-- its chunks and a text leaf its dictionary.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;

CREATE TABLE s (id int, n int, t text);
INSERT INTO s SELECT g, g % 7, 'item number ' || (g % 5000) || ' of the range ' || (g % 77) FROM generate_series(1, 60000) g;
CREATE INDEX s_col ON s USING sift_col (id, n, t);

SELECT sum(bytes) = pg_relation_size('s_col') AS adds_up FROM sift.col_sections('s_col');
SELECT section, leaf FROM sift.col_sections('s_col') WHERE section IN ('chunks', 'dict') ORDER BY section, leaf;
SELECT section FROM sift.col_sections('s_col') WHERE leaf IS NULL ORDER BY section;
SELECT bytes > 0 AS chunks_of_t, bytes < 60000 * 4 AS under_raw FROM sift.col_sections('s_col') WHERE section = 'chunks' AND leaf = 't';
SELECT bytes > 0 AS has_dictionary FROM sift.col_sections('s_col') WHERE section = 'dict' AND leaf = 't';
SELECT bytes >= 0 AS free_counted FROM sift.col_sections('s_col') WHERE section = 'free';

-- the same for a table in column format
CREATE TABLE tt (id int, n int, t text) USING sift_table;
INSERT INTO tt SELECT g, g % 7, 'item number ' || (g % 5000) || ' of the range ' || (g % 77) FROM generate_series(1, 60000) g;
VACUUM tt;
SELECT sum(bytes) = pg_relation_size('tt') AS adds_up FROM sift.col_sections('tt');
SELECT count(*) FILTER (WHERE section = 'chunks' AND leaf IN ('id', 'n', 't')) AS leaves FROM sift.col_sections('tt');

-- not a column relation
SELECT sift.col_sections('s');
CREATE INDEX s_title ON s USING sift (t);
SELECT sift.col_sections('s_title');

DROP TABLE s, tt;
DROP EXTENSION pg_sift;
