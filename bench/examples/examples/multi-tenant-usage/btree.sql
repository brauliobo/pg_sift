CREATE INDEX usage_tenant_account_idx ON usage (tenant_id, account_id);
CREATE INDEX usage_at_idx ON usage (at);
CREATE INDEX grant_row_product_idx ON grant_row (product, tenant_id, account_id);
