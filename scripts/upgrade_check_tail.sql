
SELECT status, object, kind, detected, replace(coalesce(advice, ''), E'\n', ' ')
FROM sift.upgrade_check(nullif(:'to', ''))
ORDER BY CASE status WHEN 'blocker' THEN 0 WHEN 'warn' THEN 1 WHEN 'info' THEN 2 ELSE 3 END, object;
\if :{?runbook}
SELECT :'to' = '' OR :'to' ~ '^6[.]' AS plan \gset
\if :plan
\echo
\echo The runbook, in order (pre_*: before the update, post_*: after it, info: optional); action_sql is empty where a file is edited by hand:
SELECT step, phase, severity, object, action_sql FROM sift.migrate_plan() ORDER BY step;
\endif
\endif
ROLLBACK;
