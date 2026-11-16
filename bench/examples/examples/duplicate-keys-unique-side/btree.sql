CREATE INDEX dim_id_idx ON dim (id);
CREATE INDEX fact_dim_idx ON fact (dim_id);
