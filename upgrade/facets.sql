-- Keyword facets (the design notes item 11): the number of matches of a query per value of a keyword index.
-- A keyword index is CREATE INDEX ... USING sift (col) WITH (analyzer = keyword).

-- Counts per value of facet_index among the visible rows that index matches query for, count
-- descending then value; lim NULL = every value, nulls = also a row with a NULL value counting the
-- matches without one (it makes every value be counted, so lim cannot end the search early).
CREATE FUNCTION sift.facets(index regclass, facet_index regclass, query text,
							lim int DEFAULT 10, nulls bool DEFAULT false,
							OUT value text, OUT count int8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_facet_counts' LANGUAGE C STABLE;
