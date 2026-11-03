-- ANALYZE alone of the loaded 9-column table: the sample of the statistics, a sift_table against a heap
-- label: `ANALYZE` of the table just loaded and vacuumed
--@ prepare
CREATE TABLE bulk (id bigint PRIMARY KEY, customer_id int, country text, category text, status text, amount numeric(10,2), quantity smallint, created_at timestamp, title text) USING {am};
COPY bulk FROM '{csv}' CSV;
VACUUM (FREEZE) bulk
--@ load
ANALYZE bulk
--@ check
SELECT count(*) FROM bulk
