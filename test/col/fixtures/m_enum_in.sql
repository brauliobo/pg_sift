-- IN lists and ANY of an enum, with a NULL
-- shape: expr
-- expect: agg
-- key: -
SELECT count(*) FILTER (WHERE e IN ('sad', 'meh')) AS a, count(*) FILTER (WHERE e NOT IN ('sad', 'meh')) AS b, count(*) FILTER (WHERE e = ANY (ARRAY['ok', NULL]::coldiff_mood[])) AS c FROM t_misc
