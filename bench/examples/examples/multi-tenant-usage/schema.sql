-- everything is tenant scoped: composite (tenant_id, account_id) keys, grants as scope rows, usage as the fact
CREATE TABLE tenant       (id int PRIMARY KEY, plan text NOT NULL, region text NOT NULL);
CREATE TABLE account      (tenant_id int NOT NULL, id int NOT NULL, tier int NOT NULL, status text NOT NULL, PRIMARY KEY (tenant_id, id));
CREATE TABLE grant_row (tenant_id int NOT NULL, account_id int NOT NULL, product text NOT NULL, active boolean NOT NULL, PRIMARY KEY (tenant_id, account_id, product));
CREATE TABLE usage        (id bigint PRIMARY KEY, tenant_id int NOT NULL, account_id int NOT NULL, units int NOT NULL, at timestamp NOT NULL);
