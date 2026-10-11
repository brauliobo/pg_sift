\echo Use "CREATE EXTENSION sift" to load this file. \quit

CREATE FUNCTION sift.sift_handler(internal) RETURNS index_am_handler
	AS 'MODULE_PATHNAME', 'sift_handler' LANGUAGE C STRICT;

CREATE ACCESS METHOD sift TYPE INDEX HANDLER sift.sift_handler;

CREATE FUNCTION sift.match_support(internal) RETURNS internal
	AS 'MODULE_PATHNAME', 'sift_match_support' LANGUAGE C STRICT;

CREATE FUNCTION sift.matchsel(internal, oid, internal, integer) RETURNS float8
	AS 'MODULE_PATHNAME', 'sift_matchsel' LANGUAGE C STRICT STABLE PARALLEL SAFE;

-- Default analyzer; the planner support function rebinds calls on columns
-- whose sift index uses another analyzer to sift.match_idx.
CREATE FUNCTION sift.match(doc text, query text) RETURNS bool
	AS 'MODULE_PATHNAME', 'sift_match'
	LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE COST 1000 SUPPORT sift.match_support;

CREATE FUNCTION sift.match_idx(doc text, query text, index oid) RETURNS bool
	AS 'MODULE_PATHNAME', 'sift_match_idx'
	LANGUAGE C STRICT STABLE PARALLEL SAFE COST 1000 SUPPORT sift.match_support;

-- In pg_catalog so `col @@> 'query'` works without changing search_path.
CREATE OPERATOR pg_catalog.@@> (
	LEFTARG = text,
	RIGHTARG = text,
	FUNCTION = sift.match,
	RESTRICT = sift.matchsel,
	JOIN = contjoinsel
);

CREATE OPERATOR CLASS sift.text_ops DEFAULT FOR TYPE text USING sift AS
	OPERATOR 1 pg_catalog.@@> (text, text);

-- Computed by the sift scan and projected through the plan; calling them
-- anywhere else raises an error.
CREATE FUNCTION sift.score(tid) RETURNS real
	AS 'MODULE_PATHNAME', 'sift_score' LANGUAGE C STRICT STABLE PARALLEL SAFE;

CREATE FUNCTION sift.max_score(tid) RETURNS real
	AS 'MODULE_PATHNAME', 'sift_max_score' LANGUAGE C STRICT STABLE PARALLEL SAFE;

CREATE FUNCTION sift.highlight(doc text, query text,
							   start_tag text DEFAULT '<b>', stop_tag text DEFAULT '</b>',
							   index regclass DEFAULT NULL) RETURNS text
	AS 'MODULE_PATHNAME', 'sift_highlight' LANGUAGE C STABLE PARALLEL SAFE;

-- Analyzer of index (or the default one), overridden by non-NULL options;
-- offsets are 0-based character offsets [start, end) into doc.
CREATE FUNCTION sift.tokenize(doc text, index regclass DEFAULT NULL,
							  stemmer text DEFAULT NULL, case_folding bool DEFAULT NULL,
							  accent_folding bool DEFAULT NULL,
							  OUT token text, OUT pos int4,
							  OUT start_offset int4, OUT end_offset int4)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_tokenize' LANGUAGE C STABLE PARALLEL SAFE;

CREATE FUNCTION sift.parse_query(query text, index regclass DEFAULT NULL) RETURNS text
	AS 'MODULE_PATHNAME', 'sift_parse_query' LANGUAGE C STABLE PARALLEL SAFE;

CREATE FUNCTION sift.promote(index regclass) RETURNS int4
	AS 'MODULE_PATHNAME', 'sift_promote' LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION sift.merge(index regclass) RETURNS int4
	AS 'MODULE_PATHNAME', 'sift_merge' LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION sift.segments(index regclass,
							  OUT gen int8, OUT kind text, OUT ndocs int8, OUT sum_dl int8,
							  OUT bytes int8, OUT sb_lo int4, OUT sb_hi int4, OUT dirty bool)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_segments' LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION sift.verify(index regclass, heapallindexed bool DEFAULT false) RETURNS void
	AS 'MODULE_PATHNAME', 'sift_verify' LANGUAGE C STRICT VOLATILE;
