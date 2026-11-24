-- sift.index_choices answers for a sift_col index or a sift_table of Format 2.0 (what the file holds: the layout, the tier of each level, the sections of the parts and the encodings of each column,
-- kind auto, the slug empty and the page FORMAT.md) as well as for one of Format 1 (what its CREATE INDEX derived). The signature and the Format 1 rows are those of 4.0.3.
CREATE OR REPLACE FUNCTION sift.index_choices(index regclass, OUT choice text, OUT kind text, OUT value text, OUT cost text, OUT typical_cost text, OUT why text, OUT off text, OUT slug text, OUT doc text)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_index_choices' LANGUAGE C STRICT STABLE;
COMMENT ON FUNCTION sift.index_choices(regclass) IS 'What a sift_col index or sift_table chose or advises: for Format 1 what CREATE INDEX or REINDEX derived, for Format 2.0 the layout, tiers, sections and encodings read from the file; choice, kind, value, cost measured on the index, typical cost, the reason and the command that turns it off';
