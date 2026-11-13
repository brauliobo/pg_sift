-- batch: append-logs
-- pattern: append_only
-- rows: nb
INSERT INTO log SELECT * FROM gen_log(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: service-move-team
-- pattern: dim_attr_churn
-- rows: nd / 10
UPDATE service SET team_id = 'team-' || (1 + (substr(name, 5)::int + :round) % (SELECT count(*) FROM team)) WHERE substr(name, 5)::int % 10 = :round % 10;

-- batch: team-tier-change
-- pattern: dim_attr_churn
-- rows: nd / 20
UPDATE team SET tier = 1 + (tier + :round) % 3 WHERE true;

-- batch: service-rekey
-- pattern: key_churn
-- concurrent: no
-- rows: nd / 100
UPDATE service SET id = md5('moved' || id::text || :round)::uuid WHERE substr(name, 5)::int % 100 = 1;

-- batch: services-restored
-- pattern: late_arriving_dim
-- concurrent: no
-- rows: nd / 100
INSERT INTO service SELECT * FROM gen_service(1, :nd) g WHERE substr(g.name, 5)::int % 100 = 1 AND NOT EXISTS (SELECT 1 FROM service x WHERE x.id = g.id);

-- batch: delete-logs
-- pattern: delete_heavy
-- concurrent: no
-- rows: nf / 10
DELETE FROM log WHERE id <= :nf AND id % 10 = :round % 10;

-- batch: resurrect-logs
-- pattern: delete_resurrect
-- concurrent: no
-- rows: nf / 10
INSERT INTO log SELECT * FROM gen_log(1, :nf) WHERE id % 10 = (:round - 1) % 10;

-- batch: vacuum-reindex
-- pattern: maintenance
-- maintenance: yes
-- rows: 0
VACUUM (ANALYZE) log;
REINDEX TABLE CONCURRENTLY log;
