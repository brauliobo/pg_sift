-- batch: append-usage
-- pattern: append_only
-- rows: nb
INSERT INTO usage SELECT * FROM gen_usage(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: account-tier-churn
-- pattern: dim_attr_churn
-- rows: nd / 10
UPDATE account SET tier = 1 + (tier + :round) % 4, status = (ARRAY['active', 'trial', 'closed'])[1 + (id + :round) % 3] WHERE id % 10 = :round % 10;

-- batch: tenant-plan-change
-- pattern: dim_attr_churn
-- rows: 4
UPDATE tenant SET plan = 'w' || (1 + (id + :round) % 4) WHERE id % 5 = :round % 5;

-- batch: grant_row-toggle
-- pattern: dim_attr_churn
-- rows: nd / 5
UPDATE grant_row SET active = NOT active WHERE account_id % 5 = :round % 5 AND product = 'p2';

-- batch: account-switch-tenant
-- pattern: key_churn
-- concurrent: no
-- rows: nd / 50
UPDATE account SET tenant_id = tenant_id % (SELECT count(*) FROM tenant) + 1 WHERE id % 50 = :round % 50 AND NOT EXISTS (SELECT 1 FROM grant_row s WHERE s.tenant_id = account.tenant_id AND s.account_id = account.id);

-- batch: delete-tenant-usage
-- pattern: delete_heavy
-- concurrent: no
-- rows: nf / 20
DELETE FROM usage WHERE id <= :nf AND tenant_id = 1;

-- batch: restore-tenant-usage
-- pattern: delete_resurrect
-- concurrent: no
-- rows: nf / 20
INSERT INTO usage SELECT * FROM gen_usage(1, :nf) WHERE tenant_id = 1;

-- batch: bulk-append
-- pattern: bulk_reload
-- rows: nf / 10
INSERT INTO usage SELECT * FROM gen_usage(:nf + :round * :nf + 1, :nf + :round * :nf + :nf / 10);

-- batch: vacuum-reindex
-- pattern: maintenance
-- maintenance: yes
-- rows: 0
VACUUM (ANALYZE) usage;
REINDEX TABLE CONCURRENTLY usage;
