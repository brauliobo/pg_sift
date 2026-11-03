-- VACUUM (FREEZE, ANALYZE) of the loaded and indexed 9-column table: the step of a load that settles it
-- label: `VACUUM (FREEZE, ANALYZE)` of the table just loaded, with its primary key and text index (GIN on the heap, BM25 on the `sift_table`)
--@ prepare
CREATE TABLE bulk (id bigint PRIMARY KEY, customer_id int, country text, category text, status text, amount numeric(10,2), quantity smallint, created_at timestamp, title text) USING {am};
COPY bulk FROM '{csv}' CSV;
DO $$ BEGIN IF '{am}' = 'sift_table' THEN CREATE INDEX bulk_ix ON bulk USING sift (title); ELSE CREATE INDEX bulk_ix ON bulk USING gin (to_tsvector('simple', title)); END IF; END $$
--@ load
VACUUM (FREEZE, ANALYZE) bulk
--@ check
SELECT count(*) FROM bulk
