-- sift.eval_methods names the methods under evaluation that are on: a comma separated list, a name that is none of the three is an
-- error and leaves the setting as it was. skip_trigger_lookup, the method that was on by default, is always on, and snapshot_masks, which had no code, was removed: neither is a name.
CREATE EXTENSION pg_sift;
SHOW sift.eval_methods;
SET sift.eval_methods = 'penalty_maintenance, latest_hint';
SHOW sift.eval_methods;
SET sift.eval_methods = 'penalty_maintenance,unit_rewrite';
SHOW sift.eval_methods;
SET sift.eval_methods = 'skip_trigger_lookup';
SET sift.eval_methods = 'snapshot_masks';
SET sift.eval_methods = '-unit_rewrite';
SET sift.eval_methods = 'snapshot_mask';
SHOW sift.eval_methods;
SET sift.eval_methods = 'penalty_maintenance,,';
SET sift.eval_methods = 'a b';
RESET sift.eval_methods;
SHOW sift.eval_methods;
-- the records library hashes the settings that are not the default and this one always (the query of collect.settings_of): a method that is on is in the hash
SET sift.eval_methods = 'penalty_maintenance';
SELECT name || '=' || current_setting(name) AS in_the_settings_hash FROM pg_settings WHERE (source NOT IN ('default', 'override') OR name = 'sift.eval_methods') AND name = 'sift.eval_methods';
RESET sift.eval_methods;
-- Every method is available explicitly.
SET sift.eval_methods = 'latest_hint';
SET sift.eval_methods = 'unit_rewrite';
SHOW sift.eval_methods;
SET sift.eval_methods = 'penalty_maintenance,unit_rewrite';
SHOW sift.eval_methods;
RESET sift.eval_methods;
DROP EXTENSION pg_sift;
