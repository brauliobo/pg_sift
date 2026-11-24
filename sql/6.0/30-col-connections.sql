-- sql/6.0/30-col-connections.sql: the connections of an index of format major 2. sift.col_connection_state reads the key journal of a parent index of either format, and
-- sift.col_connection_fill reports no filled segment for a child of format major 2 (it keeps no found flag). The signatures of 5.1 are unchanged.
CREATE OR REPLACE FUNCTION sift.col_connection_state(parent regclass, OUT lineage int8, OUT records int8, OUT stamp text) RETURNS record
	AS 'MODULE_PATHNAME', 'sift_col_conn_state' LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION sift.col_connection_fill(child regclass, OUT ordinal int4, OUT segments int8, OUT filled int8) RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_col_conn_fill' LANGUAGE C STRICT STABLE;
