CREATE UNIQUE INDEX member_email_idx ON member (email_lc);
CREATE INDEX contact_email_idx ON contact (lower(email));
CREATE INDEX contact_created_idx ON contact (created_at);
