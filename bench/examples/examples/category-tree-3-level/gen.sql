-- leaves = nd = nf/50 (ids 1..nd), branches = leaves/10 (ids nd+1..), roots = branches/10 (min 3, ids after); the last 10% of the leaves have no listing
SELECT greatest(:nd / 10, 10) AS nb2 \gset
SELECT greatest(:nb2 / 10, 3) AS nr \gset
CREATE FUNCTION gen_listing(lo bigint, hi bigint) RETURNS SETOF listing LANGUAGE sql AS $$
  SELECT g, gx.fkey(g, k.nf, k.nd * 9 / 10, k.s + 10, k.corr, k.skew, k.hot), gx.pick(g, k.s + 11, 100000) / 100.0,
         (ARRAY['live', 'sold', 'expired'])[gx.pick(g, k.s + 12, 3)], gx.ts(g, k.s + 13, '2024-01-01', 365) FROM generate_series(lo, hi) g, gx.knob k $$;
INSERT INTO category SELECT g, :nd + gx.pick(g, :s + 1, :nb2), 2, 'leaf' || gx.pick(g, :s + 2, :card) FROM generate_series(1, :nd) g;
INSERT INTO category SELECT :nd + g, :nd + :nb2 + gx.pick(g, :s + 3, :nr), 1, 'branch' || gx.pick(g, :s + 4, :card) FROM generate_series(1, :nb2) g;
INSERT INTO category SELECT :nd + :nb2 + g, NULL, 0, 'root' || g FROM generate_series(1, :nr) g;
INSERT INTO listing SELECT * FROM gen_listing(1, :nf);
