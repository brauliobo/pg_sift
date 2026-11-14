
SELECT status, object, kind, detected, replace(coalesce(advice, ''), E'\n', ' ')
FROM sift.upgrade_check(nullif(:'to', ''))
ORDER BY CASE status WHEN 'blocker' THEN 0 WHEN 'warn' THEN 1 ELSE 2 END, object;
ROLLBACK;
