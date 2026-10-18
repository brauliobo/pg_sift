-- The table access method sift_table (docs/COL-TAM.md). Wired into the extension version script at merge time.

CREATE FUNCTION sift.table_handler(internal) RETURNS table_am_handler
	AS 'MODULE_PATHNAME', 'sift_table_handler' LANGUAGE C STRICT;

CREATE ACCESS METHOD sift_table TYPE TABLE HANDLER sift.table_handler;
