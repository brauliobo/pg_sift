CREATE INDEX follows_col ON follows USING sift_col (follower, followee, since);
CREATE INDEX person_col ON person USING sift_col (id, country, tier);
