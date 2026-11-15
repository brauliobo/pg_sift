CREATE INDEX follows_follower_idx ON follows (follower, followee);
CREATE INDEX follows_followee_idx ON follows (followee, follower);
