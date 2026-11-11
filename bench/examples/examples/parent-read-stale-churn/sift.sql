CREATE INDEX child_col ON child USING sift_col (parent_id, amount, status, at);
CREATE INDEX parent_col ON parent USING sift_col (id, tier, opened, region, name);
