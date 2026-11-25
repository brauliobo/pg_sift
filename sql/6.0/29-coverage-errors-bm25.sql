-- BM25 key columns of any type, and several of them. A column of a type that has no operator class of its own is indexed by the text its output function writes,
-- as `col @@> 'query'` reads it. An index on several columns holds their texts as sections, which name:word queries search by the name of the column (CREATE INDEX ON docs USING sift (title, body); WHERE title @@> 'x', or WHERE sift.sections(title, body) @@> 'title:x body:y').
CREATE OR REPLACE FUNCTION sift.match_any(doc anyelement, query text) RETURNS boolean
	AS 'MODULE_PATHNAME', 'sift_match_any'
	LANGUAGE C STRICT STABLE PARALLEL SAFE COST 1000;

DO $do$
BEGIN
	IF NOT EXISTS (SELECT 1 FROM pg_operator WHERE oprname = '@@>' AND oprnamespace = 'pg_catalog'::regnamespace AND oprleft = 'anyelement'::regtype AND oprright = 'text'::regtype) THEN
		CREATE OPERATOR pg_catalog.@@> (
			LEFTARG = anyelement,
			RIGHTARG = text,
			FUNCTION = sift.match_any,
			RESTRICT = sift.matchsel,
			JOIN = contjoinsel
		);
	END IF;
	IF NOT EXISTS (SELECT 1 FROM pg_opclass WHERE opcname = 'any_ops' AND opcnamespace = 'sift'::regnamespace) THEN
		CREATE OPERATOR CLASS sift.any_ops DEFAULT FOR TYPE anyelement USING sift AS
			OPERATOR 1 pg_catalog.@@> (anyelement, text);
	END IF;
END
$do$;

COMMENT ON FUNCTION sift.match_any(anyelement, text) IS 'doc @@> query for a column of any type: the text its output function writes, matched as the text operator does.';
COMMENT ON OPERATOR pg_catalog.@@> (anyelement, text) IS 'Full-text match of a column of any type against a SiftQL query, matched on the text its output function writes; answered by a sift BM25 index.';
COMMENT ON OPERATOR CLASS sift.any_ops USING sift IS 'Lets a sift index key be a column of any type without an operator class of its own (varchar, char(n), int, uuid, jsonb, ...), indexed by its text.';
