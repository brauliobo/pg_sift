-- sift.col_maintenance_status: where the VACUUM and the rewrite that keep a sift_col index answerable stand, and what blocks them (blocked_by is empty when
-- nothing does). The statistics of a table (last VACUUM, rows changed) lag the statements that wrote them and are the TAP test's: here the table, the
-- number that starts the VACUUM and the blockers.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = on;

CREATE TABLE ms_facts (id int, key int, kind text, amount numeric(10,2)) WITH (autovacuum_enabled = false);
INSERT INTO ms_facts SELECT g, g % 100, 'cat_' || (g % 8), g % 997 FROM generate_series(1, 20000) g;
CREATE INDEX ms_idx ON ms_facts USING sift_col (id, key, kind, amount);
VACUUM ms_facts;

CREATE FUNCTION ms_status() RETURNS TABLE (heap regclass, rewrite_due text, blocked_by text[]) LANGUAGE sql AS
$$ SELECT s.heap, s.rewrite_due, s.blocked_by FROM sift.col_maintenance_status('ms_idx') s $$;

-- the setting is off, and the table opted out of autovacuum
SET sift.col_auto_merge = off;
SELECT * FROM ms_status();

-- on: the opt-out of the table is what is left
SET sift.col_auto_merge = on;
SELECT * FROM ms_status();

-- the table takes part again
ALTER TABLE ms_facts RESET (autovacuum_enabled);
SELECT * FROM ms_status();

-- no worker without sift.async_maintenance
SET sift.async_maintenance = off;
SELECT * FROM ms_status();
SET sift.async_maintenance = on;

-- the rewrite is due after churn that turns every unit dirty, and the answer is the one of sift.col_maintenance_due
UPDATE ms_facts SET amount = amount + 1 WHERE id % 20 = 1;
VACUUM ms_facts;
SELECT s.rewrite_due = sift.col_maintenance_due('ms_idx') AS same_rule, s.rewrite_due IS NOT NULL AS due
FROM sift.col_maintenance_status('ms_idx') s;

-- a table of 20,000 rows is vacuumed at its thousandth, not under 50 rows
SELECT s.vacuum_at IS NULL OR s.vacuum_at = 50 AS threshold FROM sift.col_maintenance_status('ms_idx') s;

-- not an index of sift_col
CREATE INDEX ms_btree ON ms_facts (id);
SELECT * FROM sift.col_maintenance_status('ms_btree');

DROP TABLE ms_facts;
DROP FUNCTION ms_status();
DROP EXTENSION pg_sift;
