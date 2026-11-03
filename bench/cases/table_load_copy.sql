-- COPY of the whole CSV into an empty table with its primary key, 9 columns: the step of a load that reads the file
-- label: `COPY orders FROM file` into a table with a primary key; PostgreSQL is a heap table, pg_sift a `sift_table`
--@ prepare
CREATE TABLE bulk (id bigint PRIMARY KEY, customer_id int, country text, category text, status text, amount numeric(10,2), quantity smallint, created_at timestamp, title text) USING {am}
--@ load
COPY bulk FROM '{csv}' CSV
--@ load_sift
COPY bulk FROM '{csv}' CSV;
SELECT sift.table_maintain('bulk')
--@ check
SELECT DISTINCT kind FROM sift.table_segments('bulk')
