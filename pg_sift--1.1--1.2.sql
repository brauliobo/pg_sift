\echo Use "ALTER EXTENSION pg_sift UPDATE TO '1.2'" to load this file. \quit

-- sift.segments gains extents and dead_docs; sift.index_info and sift.version are new.
DROP FUNCTION sift.segments(regclass);

-- One row per segment; the section columns are NULL for mutable segments (extents and dead_docs too).
-- dead_docs: documents of an immutable segment that VACUUM has marked dead (rows of rolled-back
-- transactions and deleted rows still count in ndocs, the document frequencies and avgdl until then).
CREATE FUNCTION sift.segments(index regclass,
							  OUT gen int8, OUT kind text, OUT ndocs int8, OUT sum_dl int8,
							  OUT avgdl float8, OUT bytes int8, OUT sb_lo int4, OUT sb_hi int4,
							  OUT dirty bool, OUT nterms int8, OUT avgdl_ref float8,
							  OUT docmap_bytes int8, OUT postings_bytes int8, OUT dict_bytes int8,
							  OUT live_bytes int8, OUT extents int4, OUT dead_docs int8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_segments' LANGUAGE C STRICT VOLATILE;


-- The space and directory state of an index. relation_blocks: size of the relation; high_water: blocks
-- ever allocated; free_blocks: blocks on the free list, of which reusable_blocks can be taken now (the
-- rest wait for the snapshots that may still read them); maintenance_busy: a promotion, merge, VACUUM or
-- verify holds the maintenance lock (NULL on a standby).
CREATE FUNCTION sift.index_info(index regclass,
								OUT relation_blocks int8, OUT high_water int8, OUT free_blocks int8,
								OUT reusable_blocks int8, OUT free_extents int8, OUT segments int4,
								OUT max_segments int4, OUT ndocs int8, OUT sum_dl int8,
								OUT maintenance_busy bool)
	RETURNS record
	AS 'MODULE_PATHNAME', 'sift_index_info' LANGUAGE C STRICT VOLATILE;

-- The version of the loaded library; the installed extension's is in pg_extension.
CREATE FUNCTION sift.version() RETURNS text
	AS 'MODULE_PATHNAME', 'sift_version' LANGUAGE C STABLE;
