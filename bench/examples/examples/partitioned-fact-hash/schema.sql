-- one dimension and a fact hash-partitioned by the join key (8 partitions): a key list or one key prunes partitions
CREATE TABLE dim  (id int PRIMARY KEY, grp text NOT NULL, state text NOT NULL, flag boolean NOT NULL, weight int NOT NULL);
CREATE TABLE fact (id bigint NOT NULL, dim_id int NOT NULL, amount numeric(12,2) NOT NULL, status text NOT NULL, at timestamp NOT NULL, PRIMARY KEY (dim_id, id)) PARTITION BY HASH (dim_id);
DO $$ BEGIN
  FOR r IN 0..7 LOOP EXECUTE format('CREATE TABLE fact_p%s PARTITION OF fact FOR VALUES WITH (MODULUS 8, REMAINDER %s)', r, r); END LOOP;
END $$;
