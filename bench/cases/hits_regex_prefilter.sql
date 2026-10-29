-- ClickBench-like: a regular expression with a class after a literal on the url
-- label: ClickBench-like: `count(*) WHERE url ~ 'google\.com/1[0-9]'`, 1,000,000 rows
-- ordered: yes
--@ pg
SELECT count(*) FROM hits WHERE url ~ 'google\.com/1[0-9]'
--@ duck
-- DuckDB's `~` matches the whole string (pg_duckdb 1.1.1 under force_execution and the DuckDB client alike: 0 against 100 on a 1,000-row table), so the pattern says so
SELECT count(*) FROM hits WHERE url ~ '.*google\.com/1[0-9].*'
