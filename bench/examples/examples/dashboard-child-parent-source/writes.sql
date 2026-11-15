-- batch: append-children
-- pattern: append_only
-- rows: nb
INSERT INTO child SELECT * FROM gen_child(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: parent-attribute-churn
-- pattern: dim_attr_churn
-- rows: nd / 10
UPDATE parent SET label = CASE WHEN label IS NULL OR label = '' THEN 'l' || id ELSE NULL END, bucket_a = 1 + (coalesce(bucket_a, 0) + :round) % 5
WHERE id % 10 = :round % 10;

-- batch: parent-move-source
-- pattern: key_churn
-- rows: nd / 20
UPDATE parent SET feed_id = 1 + (feed_id + :round) % (SELECT count(*) FROM source) WHERE id % 20 = :round % 20;

-- batch: assignment-churn
-- pattern: dim_attr_churn
-- rows: 20
DELETE FROM assignment WHERE user_id % 3 = :round % 3;
INSERT INTO assignment SELECT u, src FROM generate_series(1, 20) u, generate_series(1, (SELECT count(*) FROM source)) src
WHERE u % 3 = :round % 3 AND (gx.u(u * 100000 + src, (SELECT s + 30 FROM gx.knob) + :round) < 0.2 OR src = (u - 1) % (SELECT count(*) FROM source) + 1) ON CONFLICT DO NOTHING;

-- batch: orphan-children
-- pattern: orphan_fact
-- concurrent: no
-- rows: nb / 10
INSERT INTO child SELECT id, :nd + 1 + id % :nb, feed_id, created_at, kind, score, body FROM gen_child(:nf + :round * :nf + 1, :nf + :round * :nf + :nb / 10);

-- batch: late-arriving-parents
-- pattern: late_arriving_dim
-- concurrent: no
-- rows: nb
INSERT INTO parent SELECT * FROM gen_parent(:nd + 1, :nd + :nb);

-- batch: delete-children
-- pattern: delete_heavy
-- concurrent: no
-- rows: nf / 10
DELETE FROM child WHERE id <= :nf AND id % 10 = :round % 10;

-- batch: resurrect-children
-- pattern: delete_resurrect
-- concurrent: no
-- rows: nf / 10
INSERT INTO child SELECT * FROM gen_child(1, :nf) WHERE id % 10 = (:round - 1) % 10;

-- batch: bulk-append
-- pattern: bulk_reload
-- rows: nf / 10
INSERT INTO child SELECT * FROM gen_child(:nf + :round * :nf + 1, :nf + :round * :nf + :nf / 10);

-- batch: vacuum-reindex
-- pattern: maintenance
-- maintenance: yes
-- rows: 0
VACUUM (ANALYZE) child;
REINDEX TABLE CONCURRENTLY child;
