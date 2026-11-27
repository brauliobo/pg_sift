-- The owner of a sift_table, who is no superuser and has no right on sift.table_option_values, renames a column that its options name: the options follow the new name.
CREATE EXTENSION pg_sift;
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;
SET client_min_messages = warning;
\set VERBOSITY terse
CREATE ROLE rename_owner;
GRANT CREATE ON SCHEMA public TO rename_owner;

SET ROLE rename_owner;
CREATE TABLE events (id int, quantity int, amount int) USING sift_table;
RESET ROLE;
SELECT sift.table_set_option('events'::regclass, 'preagg', 'cube(amount: quantity)');
SELECT sift.table_set_option('events'::regclass, 'skip_index', 'amount:bloom');

SET ROLE rename_owner;
ALTER TABLE events RENAME COLUMN quantity TO qty;
ALTER TABLE events RENAME COLUMN amount TO total;
RESET ROLE;
SELECT name, value FROM sift.table_option_values WHERE rel = 'events'::regclass ORDER BY name;

DROP TABLE events;
REVOKE CREATE ON SCHEMA public FROM rename_owner;
DROP ROLE rename_owner;
DROP EXTENSION pg_sift;
