-- INSERT ... ON CONFLICT on a UNIQUE BM25 index. The executor looks for the row of the key with the equality of the operator class (strategy 3), so the classes
-- hold one: sift.===, which compares the key columns as their types do. It is no query operator: a search is col @@> 'query'.
CREATE FUNCTION sift.key_equal(a text, b text) RETURNS boolean
	AS 'MODULE_PATHNAME', 'sift_key_equal'
	LANGUAGE C STRICT STABLE PARALLEL SAFE;

CREATE FUNCTION sift.key_equal(a anyelement, b anyelement) RETURNS boolean
	AS 'MODULE_PATHNAME', 'sift_key_equal'
	LANGUAGE C STRICT STABLE PARALLEL SAFE;

CREATE OPERATOR sift.=== (LEFTARG = text, RIGHTARG = text, FUNCTION = sift.key_equal(text, text));
CREATE OPERATOR sift.=== (LEFTARG = anyelement, RIGHTARG = anyelement, FUNCTION = sift.key_equal(anyelement, anyelement));

ALTER OPERATOR FAMILY sift.text_ops USING sift ADD OPERATOR 3 sift.=== (text, text);
ALTER OPERATOR FAMILY sift.any_ops USING sift ADD OPERATOR 3 sift.=== (anyelement, anyelement);

COMMENT ON FUNCTION sift.key_equal(text, text) IS 'The equality of the key columns of a UNIQUE sift index, for INSERT ... ON CONFLICT; not a query operator.';
COMMENT ON FUNCTION sift.key_equal(anyelement, anyelement) IS 'The equality of the key columns of a UNIQUE sift index, for INSERT ... ON CONFLICT; not a query operator.';

COMMENT ON OPERATOR sift.=== (text, text) IS 'The equality of the key columns of a UNIQUE sift index, for INSERT ... ON CONFLICT; not a query operator.';
COMMENT ON OPERATOR sift.=== (anyelement, anyelement) IS 'The equality of the key columns of a UNIQUE sift index, for INSERT ... ON CONFLICT; not a query operator.';
