
CREATE OR REPLACE FUNCTION sift.index_size_breakdown(idx regclass)
RETURNS TABLE (category text, bytes bigint, pct_of_total numeric, bytes_per_row numeric)
	AS 'MODULE_PATHNAME', 'sift_index_size_breakdown' LANGUAGE C STRICT STABLE;
COMMENT ON FUNCTION sift.index_size_breakdown(regclass) IS 'Where the bytes of an index of format major 2 are: one row per category (the chunks of the granules by column class, the dictionaries, the zones, the pre-aggregates, the scope metadata, the tid index, the delete vectors, the slab, the manifest, the free blocks and the slack of the pages), which add up to pg_relation_size; the share of the index and the bytes per row of each';
