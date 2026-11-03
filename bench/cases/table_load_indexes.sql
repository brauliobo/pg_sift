-- the primary key and the text index built over the loaded 9-column table: the step of a load that builds indexes
-- label: `ALTER TABLE .. ADD PRIMARY KEY` and the text index over the loaded table (GIN of a tsvector on the heap, a BM25 index on the `sift_table`)
--@ prepare
CREATE TABLE bulk (id bigint, customer_id int, country text, category text, status text, amount numeric(10,2), quantity smallint, created_at timestamp, title text) USING {am};
COPY bulk FROM '{csv}' CSV;
VACUUM (FREEZE, ANALYZE) bulk
--@ load
ALTER TABLE bulk ADD PRIMARY KEY (id);
CREATE INDEX bulk_gin ON bulk USING gin (to_tsvector('simple', title))
--@ load_sift
ALTER TABLE bulk ADD PRIMARY KEY (id);
CREATE INDEX bulk_bm25 ON bulk USING sift (title)
--@ check
SELECT count(*) FROM bulk
