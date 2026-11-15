-- A setting that a release removed is refused with the library loaded, in a SET, ALTER ROLE and ALTER DATABASE alike, and is not there to SHOW
-- (sift.enable_col, declared in 3.0 and never read; sift.table_auto_defrag_dead_pct, without effect since 4.0). The text is the server's, for a reserved prefix.
CREATE EXTENSION pg_sift;
\set VERBOSITY default
SET sift.enable_col = off;
SET sift.table_auto_defrag_dead_pct = 1;
SHOW sift.enable_col;
SHOW sift.table_auto_defrag_dead_pct;
RESET sift.enable_col;
ALTER ROLE CURRENT_USER SET sift.enable_col = off;
SELECT format('ALTER DATABASE %I SET sift.table_auto_defrag_dead_pct = 1', current_database()) AS alter_db \gset
:alter_db;
-- the switches that stay
SET sift.enable_col_scan = off;
SET sift.enable_agg = off;
RESET sift.enable_col_scan;
RESET sift.enable_agg;
DROP EXTENSION pg_sift;
