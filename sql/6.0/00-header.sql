\echo Use "ALTER EXTENSION pg_sift UPDATE TO '6.0'" to load this file. \quit

-- 5.1 to 6.0 changes nothing that a 5.1 query, view or script names: every function and view of 5.1 stays, as itself or as a thin alias of its 6.0 successor
-- (sql/6.0/10-compat-aliases.sql, marked -- @deprecated). Each part is safe to run again.
