-- services = nd = nf/200, teams = nd/20 (min 5); uuids are md5 of the service number; messages are 'LEVEL: <code> <wide filler>' (about 160 bytes)
SELECT greatest(:nd / 20, 5) AS nt \gset
CREATE FUNCTION svc(n bigint) RETURNS uuid LANGUAGE sql IMMUTABLE PARALLEL SAFE AS $$ SELECT md5('svc' || n)::uuid $$;
CREATE FUNCTION gen_service(lo bigint, hi bigint) RETURNS SETOF service LANGUAGE sql AS $$
  SELECT svc(g), 'svc-' || g, 'team-' || gx.pick(g, k.s + 2, greatest(k.nd / 20, 5)), (ARRAY['prod', 'stage', 'dev'])[gx.pick(g, k.s + 3, 3)] FROM generate_series(lo, hi) g, gx.knob k $$;
CREATE FUNCTION gen_log(lo bigint, hi bigint) RETURNS SETOF log LANGUAGE sql AS $$
  SELECT g, svc(gx.fkey(g, k.nf, k.nd * 9 / 10, k.s + 10, k.corr, k.skew, k.hot)), l, l || ': ' || 'code' || gx.pick(g, k.s + 12, 50) || ' ' || md5(g::text) || repeat(' filler text ', 8), gx.ts(g, k.s + 13, '2024-01-01', 90)
  FROM generate_series(lo, hi) g, gx.knob k, LATERAL (SELECT (ARRAY['INFO', 'INFO', 'WARN', 'ERROR'])[gx.pick(g, k.s + 11, 4)] AS l) x $$;
INSERT INTO team SELECT 'team-' || g, 'o' || gx.pick(g, :s + 1, 8), gx.pick(g, :s + 4, 3) FROM generate_series(1, :nt) g;
INSERT INTO service SELECT * FROM gen_service(1, :nd);
INSERT INTO log SELECT * FROM gen_log(1, :nf);
