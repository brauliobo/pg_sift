-- COPY into a sift_table, written as column groups
-- label: `COPY orders FROM file` into a `sift_table`, promoted (WAL bytes, wall time and CPU per load of the orders)
--@ prepare
CREATE TABLE bulk (id bigint, customer_id int, country text, category text, status text, amount numeric(10,2), quantity smallint, created_at timestamp, title text) USING {am}
--@ load
COPY bulk FROM '{csv}' CSV
--@ load_sift
COPY bulk FROM '{csv}' CSV;
SELECT sift.table_maintain('bulk')
--@ check
SELECT DISTINCT kind FROM sift.table_segments('bulk')
