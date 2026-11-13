CREATE INDEX fact_col ON fact USING sift_col (target_id, amount, at);
CREATE INDEX entity_col ON entity USING sift_col (id, class, region);
