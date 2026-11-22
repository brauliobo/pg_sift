-- sift.arg_max(val, cmp) and sift.arg_min(val, cmp): the val of the row with the greatest (least) cmp, as ClickHouse's argMax and argMin. A row whose cmp is NULL is ignored; a NULL val on the winning row
-- is the result; a group with no row that has a cmp gives NULL. cmp is compared in the btree order of its type under the collation of the call, so it may be any type that has one (and is not the type of val).
-- The aggregates are ordinary PARALLEL SAFE aggregates, so a query that SiftAggV6 does not take (or SET sift.enable_agg = off) gives the same answer from the heap; SiftAggV6 folds them in its vector loops.
--
-- cmp is anycompatible, not anyelement: arg_max(name, created_at) is the use, and with both anyelement the two arguments would have to be of one type.

CREATE OR REPLACE FUNCTION sift_internal.argmax_trans(internal, anyelement, anycompatible) RETURNS internal
	AS 'MODULE_PATHNAME', 'sift_argmax_trans' LANGUAGE C IMMUTABLE PARALLEL SAFE SECURITY INVOKER;
CREATE OR REPLACE FUNCTION sift_internal.argmin_trans(internal, anyelement, anycompatible) RETURNS internal
	AS 'MODULE_PATHNAME', 'sift_argmin_trans' LANGUAGE C IMMUTABLE PARALLEL SAFE SECURITY INVOKER;
CREATE OR REPLACE FUNCTION sift_internal.argmax_combine(internal, internal) RETURNS internal
	AS 'MODULE_PATHNAME', 'sift_argmax_combine' LANGUAGE C IMMUTABLE PARALLEL SAFE SECURITY INVOKER;
CREATE OR REPLACE FUNCTION sift_internal.argmin_combine(internal, internal) RETURNS internal
	AS 'MODULE_PATHNAME', 'sift_argmin_combine' LANGUAGE C IMMUTABLE PARALLEL SAFE SECURITY INVOKER;
CREATE OR REPLACE FUNCTION sift_internal.argext_serial(internal) RETURNS bytea
	AS 'MODULE_PATHNAME', 'sift_argext_serial' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE SECURITY INVOKER;
CREATE OR REPLACE FUNCTION sift_internal.argext_deserial(bytea, internal) RETURNS internal
	AS 'MODULE_PATHNAME', 'sift_argext_deserial' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE SECURITY INVOKER;
CREATE OR REPLACE FUNCTION sift_internal.argext_final(internal, anyelement, anycompatible) RETURNS anyelement
	AS 'MODULE_PATHNAME', 'sift_argext_final' LANGUAGE C IMMUTABLE PARALLEL SAFE SECURITY INVOKER;

CREATE OR REPLACE AGGREGATE sift.arg_max(anyelement, anycompatible) (
	SFUNC = sift_internal.argmax_trans, STYPE = internal, FINALFUNC = sift_internal.argext_final, FINALFUNC_EXTRA, FINALFUNC_MODIFY = READ_ONLY,
	COMBINEFUNC = sift_internal.argmax_combine, SERIALFUNC = sift_internal.argext_serial, DESERIALFUNC = sift_internal.argext_deserial, PARALLEL = SAFE);
COMMENT ON AGGREGATE sift.arg_max(anyelement, anycompatible) IS 'The val of the row with the greatest cmp (ClickHouse argMax). Rows whose cmp is NULL are ignored; a NULL val on the winning row is the result; NULL when no row has a cmp. The first of the rows that tie on cmp wins.';

CREATE OR REPLACE AGGREGATE sift.arg_min(anyelement, anycompatible) (
	SFUNC = sift_internal.argmin_trans, STYPE = internal, FINALFUNC = sift_internal.argext_final, FINALFUNC_EXTRA, FINALFUNC_MODIFY = READ_ONLY,
	COMBINEFUNC = sift_internal.argmin_combine, SERIALFUNC = sift_internal.argext_serial, DESERIALFUNC = sift_internal.argext_deserial, PARALLEL = SAFE);
COMMENT ON AGGREGATE sift.arg_min(anyelement, anycompatible) IS 'The val of the row with the least cmp (ClickHouse argMin). Rows whose cmp is NULL are ignored; a NULL val on the winning row is the result; NULL when no row has a cmp. The first of the rows that tie on cmp wins.';
