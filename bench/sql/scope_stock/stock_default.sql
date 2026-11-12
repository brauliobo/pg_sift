-- the fixture's own index: a b-tree on the member, what an application has before anyone tunes it
CREATE INDEX events_dim ON events (dim_a_id);
VACUUM (ANALYZE) events;
