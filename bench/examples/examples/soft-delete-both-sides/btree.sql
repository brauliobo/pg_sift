CREATE INDEX fact_dim_live_idx ON fact (dim_id) WHERE deleted_at IS NULL;
CREATE INDEX fact_at_idx ON fact (at);
CREATE INDEX dim_state_live_idx ON dim (state) WHERE deleted_at IS NULL;
