-- M:N through a link table: event -> item <- item_tag -> tag
CREATE TABLE tag      (id int PRIMARY KEY, label text NOT NULL, grp text NOT NULL);
CREATE TABLE item     (id int PRIMARY KEY, kind text NOT NULL, keeper_id int NOT NULL);
CREATE TABLE item_tag (item_id int NOT NULL, tag_id int NOT NULL, PRIMARY KEY (item_id, tag_id));
CREATE TABLE event    (id bigint PRIMARY KEY, item_id int NOT NULL, amount numeric(12,2) NOT NULL, at timestamp NOT NULL);
