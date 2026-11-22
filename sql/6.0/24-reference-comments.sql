-- The descriptions of objects of the earlier scripts that had none: a fresh install and an update show the same \df+ and \dv+.
COMMENT ON FUNCTION sift.match_idx(text, text, oid) IS 'The function behind @@> for a column whose sift index uses another analyzer than the default one.';
COMMENT ON FUNCTION sift.col_ops_unused(anyelement) IS 'Internal: placeholder member of the col_ops operator class, never called';
COMMENT ON FUNCTION sift.facet_cap(int8) IS 'Internal: helper of sift.facet_sql, raises when a gap-filled histogram would return too many buckets';
COMMENT ON FUNCTION sift.facet_part(jsonb, int) IS 'Internal: helper of sift.facet_sql, the shape of one facet of a spec';
COMMENT ON FUNCTION sift.table_paths_check(regclass, name, text) IS 'Internal: helper of sift.table_set_paths, checks that the table can hold the paths';
COMMENT ON VIEW sift.col_rollup_state IS 'The columns of sift.col_rollup_counters as a view.';
COMMENT ON VIEW sift.col_rollup_requests IS 'The rows of sift.col_rollup_queue as a view.';
COMMENT ON TABLE sift.table_paths IS 'The declared JSON paths of the jsonb columns of sift_table tables, one row per table and column, dumped with the extension.';
