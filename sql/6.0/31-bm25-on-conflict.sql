-- INSERT ... ON CONFLICT on a UNIQUE BM25 index. The executor looks for the row of the key with the equality of the operator class (strategy 3), so the classes
-- hold one: sift.===, which compares the key columns as their types do. It is no query operator: a search is col @@> 'query'.
CREATE OR REPLACE FUNCTION sift.key_equal(a text, b text) RETURNS boolean
	AS 'MODULE_PATHNAME', 'sift_key_equal'
	LANGUAGE C STRICT STABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION sift.key_equal(a anyelement, b anyelement) RETURNS boolean
	AS 'MODULE_PATHNAME', 'sift_key_equal'
	LANGUAGE C STRICT STABLE PARALLEL SAFE;

DO $do$
BEGIN
	IF NOT EXISTS (SELECT 1 FROM pg_operator WHERE oprname = '===' AND oprnamespace = 'sift'::regnamespace AND oprleft = 'text'::regtype AND oprright = 'text'::regtype) THEN
		CREATE OPERATOR sift.=== (LEFTARG = text, RIGHTARG = text, FUNCTION = sift.key_equal(text, text));
	END IF;
	IF NOT EXISTS (SELECT 1 FROM pg_operator WHERE oprname = '===' AND oprnamespace = 'sift'::regnamespace AND oprleft = 'anyelement'::regtype AND oprright = 'anyelement'::regtype) THEN
		CREATE OPERATOR sift.=== (LEFTARG = anyelement, RIGHTARG = anyelement, FUNCTION = sift.key_equal(anyelement, anyelement));
	END IF;
	IF NOT EXISTS (SELECT 1 FROM pg_amop WHERE amopstrategy = 3 AND amoplefttype = 'text'::regtype AND amoprighttype = 'text'::regtype
											 AND amopopr = 'sift.===(text, text)'::regoperator) THEN
		ALTER OPERATOR FAMILY sift.text_ops USING sift ADD OPERATOR 3 sift.=== (text, text);
	END IF;
	IF NOT EXISTS (SELECT 1 FROM pg_amop WHERE amopstrategy = 3 AND amoplefttype = 'anyelement'::regtype AND amoprighttype = 'anyelement'::regtype
											 AND amopopr = 'sift.===(anyelement, anyelement)'::regoperator) THEN
		ALTER OPERATOR FAMILY sift.any_ops USING sift ADD OPERATOR 3 sift.=== (anyelement, anyelement);
	END IF;
END
$do$;

COMMENT ON FUNCTION sift.key_equal(text, text) IS 'The equality of the key columns of a UNIQUE sift index, for INSERT ... ON CONFLICT; not a query operator.';
COMMENT ON FUNCTION sift.key_equal(anyelement, anyelement) IS 'The equality of the key columns of a UNIQUE sift index, for INSERT ... ON CONFLICT; not a query operator.';

COMMENT ON OPERATOR sift.=== (text, text) IS 'The equality of the key columns of a UNIQUE sift index, for INSERT ... ON CONFLICT; not a query operator.';
COMMENT ON OPERATOR sift.=== (anyelement, anyelement) IS 'The equality of the key columns of a UNIQUE sift index, for INSERT ... ON CONFLICT; not a query operator.';
