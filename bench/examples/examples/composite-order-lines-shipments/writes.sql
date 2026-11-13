-- batch: new-orders
-- pattern: append_only
-- rows: nb
INSERT INTO header SELECT * FROM gen_header((:nf + :round * :nf) / 4 + 1, (:nf + :round * :nf + :nb) / 4);
INSERT INTO order_line SELECT * FROM gen_line(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);
INSERT INTO shipment SELECT * FROM gen_shipment(:nf + :round * :nf + 1, :nf + :round * :nf + :nb);

-- batch: ship-late
-- pattern: late_arriving_dim
-- concurrent: no
-- rows: nb / 4
DELETE FROM shipment WHERE order_id > :nf / 4 AND line_no = 4 AND order_id % 25 = 0;
INSERT INTO shipment SELECT * FROM gen_shipment(:nf + 1, :nf + :nb) g WHERE NOT EXISTS (SELECT 1 FROM shipment x WHERE x.order_id = g.order_id AND x.line_no = g.line_no);

-- batch: carrier-reassign
-- pattern: dim_attr_churn
-- rows: nf / 40
UPDATE shipment SET carrier = 'w' || (1 + (order_id + :round) % :card) WHERE order_id % 10 = :round % 10;

-- batch: header-status-change
-- pattern: dim_attr_churn
-- rows: nd / 10
UPDATE header SET status = (ARRAY['open', 'paid', 'closed'])[1 + (id + :round) % 3] WHERE id % 10 = :round % 10;

-- batch: lines-renumber
-- pattern: key_churn
-- concurrent: no
-- rows: nd / 50
UPDATE order_line SET line_no = line_no + 10 WHERE order_id <= :nf / 4 AND order_id % 50 = :round % 50;

-- batch: delete-shipments
-- pattern: delete_heavy
-- concurrent: no
-- rows: nf / 10
DELETE FROM shipment WHERE order_id <= :nf / 4 AND order_id % 10 = 7;

-- batch: reship
-- pattern: delete_resurrect
-- concurrent: no
-- rows: nf / 10
INSERT INTO shipment SELECT * FROM gen_shipment(1, :nf) g WHERE g.order_id % 10 = 7 AND NOT EXISTS (SELECT 1 FROM shipment x WHERE x.order_id = g.order_id AND x.line_no = g.line_no);

-- batch: vacuum-reindex
-- pattern: maintenance
-- maintenance: yes
-- rows: 0
VACUUM (ANALYZE) order_line;
REINDEX TABLE CONCURRENTLY order_line;
