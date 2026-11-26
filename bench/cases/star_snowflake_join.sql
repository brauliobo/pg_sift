-- a snowflake join (the fact table to a product, the product to its category) and a flat dimension (the region) under one grouped aggregate: the lookups are hashed at the start and read beside the scan of the fact table
-- label: `sf_events JOIN sf_product USING (prod_id) JOIN sf_category USING (cat_id) JOIN sf_region USING (reg_id) WHERE r.active AND e.day >= date '2024-06-01'`, `sum(e.qty * p.price)` grouped by the category and the tier
-- ordered: yes
--@ pg
SELECT c.label, r.tier, count(*), sum(e.qty * p.price) FROM sf_events e JOIN sf_product p USING (prod_id) JOIN sf_category c USING (cat_id) JOIN sf_region r USING (reg_id)
WHERE r.active AND e.day >= date '2024-06-01' GROUP BY 1, 2 ORDER BY 1, 2
