
-- The layouts of a sift_table: its own order (the option key_order) and how far its rows are from it.
CREATE FUNCTION sift.table_layouts(tbl regclass,
	OUT layout text, OUT kind text, OUT key_order text, OUT bytes bigint, OUT rows bigint, OUT units bigint, OUT units_per_key float8, OUT restore text)
	RETURNS SETOF record AS 'MODULE_PATHNAME', 'sift_table_layouts' LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION sift.table_layouts(regclass) IS 'One row per layout of a sift_table: the table itself (kind primary, its key_order or load order), its size, and for a key_order on an integer, date or timestamp column the rows and units of its column groups, the units an equality on the first key column reads on average (1 when sorted) and what puts the rows back in order when they have drifted';
