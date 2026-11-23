-- BM25 key columns of any type, and several of them. A column of a type that has no operator class of its own is indexed by the text its output function writes,
-- as `col @@> 'query'` reads it. An index on several columns holds their texts as sections, which name:word queries search by the name of the column (CREATE INDEX ON docs USING sift (title, body); WHERE title @@> 'x', or WHERE sift.sections(title, body) @@> 'title:x body:y').
CREATE FUNCTION sift.match_any(doc anyelement, query text) RETURNS boolean
	AS 'MODULE_PATHNAME', 'sift_match_any'
	LANGUAGE C STRICT STABLE PARALLEL SAFE COST 1000;

CREATE OPERATOR pg_catalog.@@> (
	LEFTARG = anyelement,
	RIGHTARG = text,
	FUNCTION = sift.match_any,
	RESTRICT = sift.matchsel,
	JOIN = contjoinsel
);

CREATE OPERATOR CLASS sift.any_ops DEFAULT FOR TYPE anyelement USING sift AS
	OPERATOR 1 pg_catalog.@@> (anyelement, text);

COMMENT ON FUNCTION sift.match_any(anyelement, text) IS 'doc @@> query for a column of any type: the text its output function writes, matched as the text operator does.';
COMMENT ON OPERATOR CLASS sift.any_ops USING sift IS 'Lets a sift index key be a column of any type without an operator class of its own (varchar, char(n), int, uuid, jsonb, ...), indexed by its text.';
