-- The typed operator classes of sift_col are the default of their types, so that CREATE [UNIQUE] INDEX ... USING sift_col (id) binds sift.col_int8_ops and INSERT ... ON CONFLICT (id) finds
-- the equality of the key (PostgreSQL asks the class of the index for a btree strategy; sift.col_ops, the class of anyelement, has none). sift.col_ops stays the default of every other type.
-- The stored index is the one of sift.col_ops whatever the class, and a class is no part of what an index of format major 1 reads, so the indexes that exist keep the class they were made with.
-- Left out: sift.col_numeric_ops (numeric(p, s) with p up to 18 only) and sift.col_bpchar_ops (char(n) only), which refuse the columns of other types the default class takes today; varchar takes
-- sift.col_text_ops (binary coercible); jsonb and arrays keep their opt-in classes (no equality). A text column under a nondeterministic collation names sift.col_ops: sift.col_text_ops refuses it.
-- Safe to run again.
UPDATE pg_catalog.pg_opclass SET opcdefault = true
WHERE opcmethod = (SELECT oid FROM pg_catalog.pg_am WHERE amname = 'sift_col')
  AND opcnamespace = 'sift'::regnamespace
  AND opcname IN ('col_int2_ops', 'col_int4_ops', 'col_int8_ops', 'col_float4_ops', 'col_float8_ops', 'col_date_ops', 'col_timestamp_ops', 'col_timestamptz_ops', 'col_bool_ops',
				  'col_text_ops', 'col_uuid_ops')
  AND NOT opcdefault;
