
-- Operator class that lets the planner answer a jsonb ? 'key' on a column of a sift_col index of format major 2 from the index: the granules keep which rows have the key of a
-- declared top-level path (docs/COL-SCAN.md). Opt-in per column, like the classes above: CREATE INDEX ON t USING sift_col (id, doc sift.col_jsonb_ops) WITH (paths = 'doc.flag_a astext').
DO $$
BEGIN
	IF NOT EXISTS (SELECT 1 FROM pg_opclass WHERE opcname = 'col_jsonb_ops' AND opcnamespace = 'sift'::regnamespace) THEN
		CREATE OPERATOR CLASS sift.col_jsonb_ops FOR TYPE jsonb USING sift_col AS OPERATOR 30 pg_catalog.? (jsonb, text);
	END IF;
END $$;
COMMENT ON OPERATOR CLASS sift.col_jsonb_ops USING sift_col IS 'Lets the planner answer jsonb ? text on a column of a sift_col index of format major 2 from the index, for the top-level paths the paths option declares.';
