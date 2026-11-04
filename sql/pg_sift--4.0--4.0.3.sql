\echo Use "ALTER EXTENSION pg_sift UPDATE TO '4.0.3'" to load this file. \quit

-- What CREATE INDEX and REINDEX of a sift_col index chose or advise, one row each: the name (choice), kind (auto: a choice the index holds, advice, limit: where a cap of an option left units
-- without what was asked, action: something to do), value, cost measured on this index (NULL when the build measured none), typical cost from the benchmark of the option (NULL for advice),
-- the reason, the command that turns it off, the slug that sift.help(slug) takes and the page of docs/ that explains it. The NOTICE of the statement counts the same rows.
CREATE FUNCTION sift.index_choices(index regclass, OUT choice text, OUT kind text, OUT value text, OUT cost text, OUT typical_cost text, OUT why text, OUT off text, OUT slug text, OUT doc text)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_col_index_choices' LANGUAGE C STRICT STABLE;

-- The row of docs/messages.tsv of a message or a choice of the NOTICE (slug), or of all of them without an argument: the message, the detail and hint, what pg_sift decided, why, the command
-- that changes it and the page of docs/ (docs/MESSAGES.md has the same rows).
CREATE FUNCTION sift.help(slug text DEFAULT NULL, OUT id text, OUT kind text, OUT level text, OUT message text, OUT detail text, OUT hint text, OUT decision text, OUT why text, OUT off text, OUT doc text)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_help' LANGUAGE C STABLE;

COMMENT ON FUNCTION sift.index_choices(regclass) IS 'What CREATE INDEX or REINDEX of a sift_col index chose or advises: choice, kind, value, cost measured on the index, typical cost, the reason and the command that turns it off';
COMMENT ON FUNCTION sift.help(text) IS 'The row of docs/messages.tsv of a pg_sift message or choice by its slug (all rows without an argument)';
