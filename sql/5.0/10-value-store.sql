
-- The shared value store of a column: one relation for each (table, column) that several sift_col indexes of the table share (docs/COL-VSTORE.md)
CREATE FUNCTION sift.values_handler(internal) RETURNS table_am_handler
	AS 'MODULE_PATHNAME', 'sift_values_handler' LANGUAGE C STRICT;
CREATE ACCESS METHOD sift_values TYPE TABLE HANDLER sift.values_handler;

CREATE VIEW sift.col_value_store_status AS
SELECT s.oid::regclass AS store, t.oid::regclass AS table_name, a.attname AS column_name,
       pg_relation_size(s.oid) AS bytes,
       (SELECT count(*) FROM pg_depend d WHERE d.classid = 'pg_class'::regclass AND d.refclassid = 'pg_class'::regclass
          AND d.refobjid = s.oid AND d.deptype = 'n') AS indexes_using
  FROM pg_class s
  JOIN pg_am am ON am.oid = s.relam AND am.amname = 'sift_values'
  JOIN pg_attribute a ON a.attrelid = s.oid AND a.attnum = 1
  LEFT JOIN pg_depend dt ON dt.classid = 'pg_class'::regclass AND dt.objid = s.oid AND dt.objsubid = 0
       AND dt.refclassid = 'pg_class'::regclass AND dt.deptype = 'a'
  LEFT JOIN pg_class t ON t.oid = dt.refobjid;
COMMENT ON VIEW sift.col_value_store_status IS 'The shared value stores (relations <table>_<column>_sift_values, managed by sift): the table and column each one serves, its size and how many indexes use it';

