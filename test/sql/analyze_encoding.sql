-- SQL_ASCII: bytes >= 0x80 are opaque letters, ASCII still folds
SELECT current_database() AS orig \gset
CREATE DATABASE sift_sql_ascii ENCODING 'SQL_ASCII' LOCALE 'C' TEMPLATE template0;
CREATE DATABASE sift_latin1 ENCODING 'LATIN1' LOCALE 'C' TEMPLATE template0;
\c sift_sql_ascii
CREATE EXTENSION sift;
SELECT token::bytea, pos, start_offset, end_offset
  FROM sift.tokenize(E'Caf\xe9 NA\xcfVE \xff\xfe-x \xc3\xa9T\xc3\xa9');
SELECT octet_length(token) FROM sift.tokenize(repeat(E'\xe9', 300));
\c sift_latin1
CREATE EXTENSION sift;
SELECT * FROM sift.tokenize('x');
\c :orig
DROP DATABASE sift_sql_ascii;
DROP DATABASE sift_latin1;
