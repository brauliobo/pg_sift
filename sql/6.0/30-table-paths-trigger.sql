-- The declared paths of a sift_table of Format 2.0 are leaves of its store, so a change of sift.table_paths, by whichever statement (sift.table_set_paths, a rename of a column, a restore), writes out
-- what the table holds under the old declaration and replaces its store in every backend. A table of Format 1 reads its declaration with its cache entry and is left alone.
CREATE FUNCTION sift.table_paths_trg() RETURNS trigger
	AS 'MODULE_PATHNAME', 'sift_table_paths_trg' LANGUAGE C;
CREATE TRIGGER table_paths_trg BEFORE INSERT OR UPDATE OR DELETE ON sift.table_paths
	FOR EACH ROW EXECUTE FUNCTION sift.table_paths_trg();
COMMENT ON FUNCTION sift.table_paths_trg() IS 'Trigger of sift.table_paths: a sift_table of Format 2.0 writes out the rows it holds under the old declaration of paths and replaces its store in every backend.';
