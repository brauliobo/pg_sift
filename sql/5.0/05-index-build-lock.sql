
CREATE FUNCTION sift.col_build_lock() RETURNS event_trigger
	AS 'MODULE_PATHNAME', 'sift_col_build_lock' LANGUAGE C;
CREATE EVENT TRIGGER sift_col_build_lock ON ddl_command_start WHEN TAG IN ('CREATE INDEX') EXECUTE FUNCTION sift.col_build_lock();
COMMENT ON FUNCTION sift.col_build_lock() IS 'Internal: the event trigger before CREATE INDEX of a sift_col index; takes the table lock that the build of its change log needs first, so that two builds on one table do not deadlock';
