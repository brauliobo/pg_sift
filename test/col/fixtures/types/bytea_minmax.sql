-- bytea min and max (the aggregates exist from PostgreSQL 18)
-- shape: expr
-- expect: agg
-- key: -
-- requires: SELECT current_setting('server_version_num')::int >= 180000
SELECT encode(min(b), 'hex') AS mn, encode(max(b), 'hex') AS mx FROM t_misc
