
-- sift.table_option_values had no replica identity: with a publication that publishes deletes (FOR ALL TABLES includes the table) its sql_drop trigger made every DROP fail.
-- The unique index on its two NOT NULL key columns is the identity. The 4.3.1 update runs the same statement, which changes nothing a second time.
ALTER TABLE sift.table_option_values REPLICA IDENTITY USING INDEX table_option_values_key;
