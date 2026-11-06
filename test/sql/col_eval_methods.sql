-- sift.eval_methods names the methods under evaluation that are on: a comma separated list; penalty_maintenance is the only name. A name that is none of it is an
-- error and leaves the setting as it was, and so are the names of the four methods removed in 4.2 (skip_trigger_lookup is always on, snapshot_masks had no code, latest_hint
-- won nothing, the writer of unit_rewrite is gone) and the sign of -name.
CREATE EXTENSION pg_sift;
SHOW sift.eval_methods;
SET sift.eval_methods = 'penalty_maintenance';
SHOW sift.eval_methods;
SET sift.eval_methods = 'penalty_maintenance, penalty_maintenance';
SHOW sift.eval_methods;
SET sift.eval_methods = 'skip_trigger_lookup';
SET sift.eval_methods = 'snapshot_masks';
SET sift.eval_methods = 'latest_hint';
SET sift.eval_methods = 'unit_rewrite';
SET sift.eval_methods = '-penalty_maintenance';
SET sift.eval_methods = 'snapshot_mask';
SET sift.eval_methods = 'penalty_maintenance,,';
SET sift.eval_methods = 'a b';
SHOW sift.eval_methods;
RESET sift.eval_methods;
SHOW sift.eval_methods;
-- the records library hashes the settings that are not the default and this one always (the query of collect.settings_of): a method that is on is in the hash
SET sift.eval_methods = 'penalty_maintenance';
SELECT name || '=' || current_setting(name) AS in_the_settings_hash FROM pg_settings WHERE (source NOT IN ('default', 'override') OR name = 'sift.eval_methods') AND name = 'sift.eval_methods';
RESET sift.eval_methods;
DROP EXTENSION pg_sift;
