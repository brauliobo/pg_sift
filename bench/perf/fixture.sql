CREATE EXTENSION pg_sift;
SET sift.async_maintenance=off;
SET sift.table_write_buffer_limit=2048;
CREATE TABLE seed (id int, g int, amount bigint, body text, pad text);
ALTER TABLE seed ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO seed
SELECT i,i%16,CASE WHEN i%31=0 THEN NULL ELSE i%1000 END,
       'common wire ' || CASE WHEN i%97=3 THEN 'rare ' ELSE '' END ||
       repeat('clean ',1+i%13) || 'word' || i%43,(SELECT string_agg(md5(i::text || ':' || j::text),'') FROM generate_series(1,48) j)
FROM generate_series(1,:rows) i;
CREATE TABLE docs (LIKE seed);
ALTER TABLE docs ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO docs SELECT * FROM seed ORDER BY id;
CREATE INDEX docs_bm25 ON docs USING sift(body);
SELECT EXISTS (SELECT 1 FROM pg_am WHERE amname='sift_col') AS has_col, EXISTS (SELECT 1 FROM pg_am WHERE amname='sift_table') AS has_table \gset
\if :has_col
CREATE INDEX docs_col ON docs USING sift_col(id,g,amount);
\endif
\if :has_table
CREATE TABLE store (LIKE seed) USING sift_table;
INSERT INTO store SELECT * FROM seed ORDER BY id;
CREATE INDEX store_pk ON store(id);
CREATE TABLE mutable (LIKE seed) USING sift_table;
CREATE INDEX mutable_pk ON mutable(id);
\endif
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) docs;
\if :has_table
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) store;
\endif
ANALYZE seed;
