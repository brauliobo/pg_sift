-- The typed operator classes of sift_col are the default of their types, so that CREATE [UNIQUE] INDEX ... USING sift_col (id) binds sift.col_int8_ops and INSERT ... ON CONFLICT (id) finds
-- the equality of the key (PostgreSQL asks the class of the index for a btree strategy; sift.col_ops, the class of anyelement, has none). sift.col_ops stays the default of every other type.
-- The stored index is the one of sift.col_ops whatever the class, and a class is no part of what an index of format major 1 reads, so the indexes that exist keep the class they were made with.
-- numeric (any precision, NaN and infinities), char(n) and bpchar bind their typed classes too, and a text column under a nondeterministic collation binds sift.col_text_ops: the scan decides
-- what the leaf holds exactly (a narrow numeric, the bytes of a text) and leaves the rest to the heap. varchar takes sift.col_text_ops (binary coercible); jsonb and arrays keep their opt-in classes (no equality).
-- Safe to run again.
UPDATE pg_catalog.pg_opclass SET opcdefault = true
WHERE opcmethod = (SELECT oid FROM pg_catalog.pg_am WHERE amname = 'sift_col')
  AND opcnamespace = 'sift'::regnamespace
  AND opcname IN ('col_int2_ops', 'col_int4_ops', 'col_int8_ops', 'col_float4_ops', 'col_float8_ops', 'col_date_ops', 'col_timestamp_ops', 'col_timestamptz_ops', 'col_bool_ops',
				  'col_text_ops', 'col_uuid_ops', 'col_numeric_ops', 'col_bpchar_ops')
  AND NOT opcdefault;
