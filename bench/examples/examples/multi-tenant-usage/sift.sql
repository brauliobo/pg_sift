CREATE INDEX usage_col ON usage USING sift_col (tenant_id, account_id, units, at);
CREATE INDEX account_col ON account USING sift_col (tenant_id, id, tier, status);
CREATE INDEX grant_row_col ON grant_row USING sift_col (tenant_id, account_id, product, active);
CREATE INDEX tenant_col ON tenant USING sift_col (id, plan, region);
