-- COPY into a sift_table, written as column groups, against a heap COPY
-- label: `COPY orders FROM file` of the whole CSV, promoted; PostgreSQL is a heap table, pg_sift a `sift_table` with `sift.table_bulk_groups` on (the default), `bulk_off` the same `sift_table` with it off
--@ prepare
CREATE TABLE bulk (id bigint, customer_id int, country text, category text, status text, amount numeric(10,2), quantity smallint, created_at timestamp, title text) USING {am}
--@ load
COPY bulk FROM '{csv}' CSV
--@ load_sift
COPY bulk FROM '{csv}' CSV;
SELECT sift.table_maintain('bulk')
--@ check
SELECT DISTINCT kind FROM sift.table_segments('bulk')
