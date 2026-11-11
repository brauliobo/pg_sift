-- batch: append-orders
-- pattern: append_only
-- rows: nb
INSERT INTO orders SELECT * FROM gen_orders(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: customer-attribute-churn
-- pattern: dim_attr_churn
-- rows: nd / 10
UPDATE customer SET segment = 'w' || (1 + (id + :round) % :card), tier = 1 + (tier + :round) % 5 WHERE id % 10 = :round % 10;

-- batch: item-recategorize
-- pattern: dim_attr_churn
-- rows: nd / 35
UPDATE item SET category = 'w' || (1 + (id + :round) % :card) WHERE id % 7 = :round % 7;

-- batch: orders-change-customer
-- pattern: key_churn
-- rows: nf / 50
UPDATE orders SET customer_id = 1 + (customer_id + :round) % :nd WHERE id <= :nf AND id % 50 = :round % 50;

-- batch: customer-renumber
-- pattern: key_churn
-- concurrent: no
-- rows: nd / 100
UPDATE customer SET id = id + 1000000000 WHERE id % 100 = 1 AND id <= :nd;

-- batch: late-arriving-customers
-- pattern: late_arriving_dim
-- concurrent: no
-- rows: nd / 100
INSERT INTO customer SELECT * FROM gen_customer(1, :nd) WHERE id % 100 = 1;

-- batch: delete-orders
-- pattern: delete_heavy
-- concurrent: no
-- rows: nf / 10
DELETE FROM orders WHERE id <= :nf AND id % 10 = :round % 10;

-- batch: resurrect-orders
-- pattern: delete_resurrect
-- concurrent: no
-- rows: nf / 10
INSERT INTO orders SELECT * FROM gen_orders(1, :nf) WHERE id % 10 = (:round - 1) % 10;

-- batch: bulk-append
-- pattern: bulk_reload
-- rows: nf / 10
INSERT INTO orders SELECT * FROM gen_orders(:nf + :round * :nf + 1, :nf + :round * :nf + :nf / 10);

-- batch: vacuum-reindex
-- pattern: maintenance
-- maintenance: yes
-- rows: 0
VACUUM (ANALYZE) orders;
REINDEX TABLE CONCURRENTLY orders;
