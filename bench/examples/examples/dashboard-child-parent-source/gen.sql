-- parents = nd = nf/20 (the last 10% have no child), sources = nd/100 (min 5), 20 users each assigned about 20% of the sources; null_pct percent of labels, bodies are NULL and as many are ''.
-- The first 24 parents repeat the (feed_id, external_key) pair of parent id + 24: 24 duplicate pairs at every scale.
SELECT greatest(:nd / 100, 5) AS ns \gset
CREATE FUNCTION gen_parent(lo bigint, hi bigint) RETURNS SETOF parent LANGUAGE sql AS $$
  SELECT g, gx.pick(q, k.s + 2, greatest(k.nd / 100, 5)), 'k' || q,
         CASE WHEN gx.isnull(g, k.s + 3, k.null_pct) THEN NULL WHEN gx.isnull(g, k.s + 4, k.null_pct) THEN '' ELSE 'l' || gx.pick(g, k.s + 5, 1000) END,
         CASE WHEN gx.isnull(g, k.s + 6, k.null_pct / 2) THEN NULL ELSE 'c' || gx.pick(g, k.s + 7, 50) END,
         CASE WHEN gx.isnull(g, k.s + 8, k.null_pct) THEN NULL ELSE gx.pick(g, k.s + 9, 5) END, gx.u(g, k.s + 10) < 0.3
  FROM generate_series(lo, hi) g, gx.knob k, LATERAL (SELECT CASE WHEN g <= 24 THEN g + 24 ELSE g END AS q) x $$;
CREATE FUNCTION gen_child(lo bigint, hi bigint) RETURNS SETOF child LANGUAGE sql AS $$
  SELECT g, p, gx.pick(CASE WHEN p <= 24 THEN p + 24 ELSE p END, k.s + 2, greatest(k.nd / 100, 5)), gx.ts(g, k.s + 11, '2024-01-01', 365), 'k' || gx.pick(g, k.s + 12, 4),
         CASE WHEN gx.isnull(g, k.s + 13, k.null_pct) THEN NULL ELSE gx.pick(g, k.s + 14, 5) END,
         CASE WHEN gx.isnull(g, k.s + 15, k.null_pct) THEN NULL WHEN gx.isnull(g, k.s + 16, k.null_pct) THEN '' ELSE 'text' || gx.pick(g, k.s + 17, 100) END
  FROM generate_series(lo, hi) g, gx.knob k, LATERAL (SELECT gx.fkey(g, k.nf, k.nd * 9 / 10, k.s + 18, k.corr, k.skew, k.hot)::bigint AS p) x $$;
INSERT INTO source SELECT g, gx.word(g, :s + 1, 6), 'r' || gx.pick(g, :s + 19, 5) FROM generate_series(1, :ns) g;
INSERT INTO parent SELECT * FROM gen_parent(1, :nd);
INSERT INTO child SELECT * FROM gen_child(1, :nf);
INSERT INTO assignment SELECT u, src FROM generate_series(1, 20) u, generate_series(1, :ns) src WHERE gx.u(u * 100000 + src, :s + 20) < 0.2 OR src = (u - 1) % :ns + 1;
