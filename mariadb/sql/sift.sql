-- Queue, triggers, drain, rebuild and search procedures of a sift index (load after functions.sql, in the database of the
-- indexed table).
--
--   CALL sift_attach('wiki', 'docs', 'id', 't.body', 32);   -- index docs.body: creates the engine index, the queue table
--                                                            -- sift_q_wiki, the triggers, and queues the existing rows
--   CALL sift_attach_opts('wiki', 'docs', 'id', 't.body', '{"stemmer":"en","positions":false,"prefix_index":3}');
--   CALL sift_attach_opts('docs', 'docs', 'a,b', 't.title', '{"fields":[{"name":"title","expr":"t.title"},{"name":"body","expr":"t.body"}]}');
--   CALL sift_sync('wiki');                                  -- moves the queue into a segment (the index is read fresh anyway)
--   SELECT d.id, j.s FROM (SELECT sift_topk('wiki', 'a AND b', 10) AS r) x, JSON_TABLE(x.r, '$[*]'
--          COLUMNS (n FOR ORDINALITY, id BIGINT PATH '$.id', s DOUBLE PATH '$.s')) j JOIN docs d ON d.id = j.id ORDER BY j.n;
--   CALL sift_search('wiki', 'a AND b', 10, 't.likes > 3');  -- the same with a filter pushed into the ranked scan
--   CALL sift_facets('wiki', 'a', 't.category', NULL);       -- counts of the matches per value, as GROUP BY
--   CALL sift_prune('wiki', 3600); CALL sift_rebuild('wiki'); CALL sift_schedule('wiki', 1); CALL sift_detach('wiki');
--
-- The text expression is written over the alias t of the indexed table (t.body, CONCAT_WS(' ', t.title, t.body)). The key is
-- the primary key: one integer column, or text, uuid or several columns, which a key table (sift_k_<name>, kept by the
-- triggers) maps to dense integers; the functions then return those integers and sift_search joins them back. The
-- triggers only queue (op, key) in the writer's transaction: a rolled-back write leaves no trace. The queue is a log:
-- a drain reads the rows above the watermark of the node and below its newest barrier, and deletes nothing (sift_prune
-- does, by age), so a replica that replicates the queue drains its own copy without racing the primary's deletes.
DELIMITER //

CREATE TABLE IF NOT EXISTS sift_indexes (
	name       VARCHAR(64) NOT NULL PRIMARY KEY,
	db         VARCHAR(64) NOT NULL,
	tbl        VARCHAR(64) NOT NULL,
	pkcols     TEXT NOT NULL,
	keymode    VARCHAR(3) NOT NULL,		-- int: the key is the integer primary key, map: through sift_k_<name>
	qname      TEXT NOT NULL,
	keyexpr    TEXT NOT NULL,
	source     TEXT NOT NULL,
	drain_from TEXT NOT NULL,
	present    TEXT NOT NULL,			-- true when the row of the table exists in a drain row
	textexpr   TEXT NOT NULL,
	cfg        TEXT NOT NULL,
	pruned_to  BIGINT NOT NULL DEFAULT 0
) ENGINE=InnoDB//

DROP PROCEDURE IF EXISTS sift_run//
CREATE PROCEDURE sift_run(IN sql_text TEXT)
BEGIN
	SET @sift_sql = sql_text;
	PREPARE sift_st FROM @sift_sql;
	EXECUTE sift_st;
	DEALLOCATE PREPARE sift_st;
END//

-- The columns of a comma separated list written through a format: sift_colcat('a,b', 't.{c} = k.{c}', ' AND ')
DROP FUNCTION IF EXISTS sift_colcat//
CREATE FUNCTION sift_colcat(p_cols TEXT, p_fmt TEXT, p_sep VARCHAR(20)) RETURNS TEXT DETERMINISTIC
BEGIN
	DECLARE v_out TEXT DEFAULT '';
	DECLARE v_i INT DEFAULT 1;
	DECLARE v_n INT DEFAULT 1 + LENGTH(p_cols) - LENGTH(REPLACE(p_cols, ',', ''));

	WHILE v_i <= v_n DO
		SET v_out = CONCAT(v_out, IF(v_i > 1, p_sep, ''),
			REPLACE(p_fmt, '{c}', CONCAT('`', TRIM(SUBSTRING_INDEX(SUBSTRING_INDEX(p_cols, ',', v_i), ',', -1)), '`')));
		SET v_i = v_i + 1;
	END WHILE;
	RETURN v_out;
END//

DROP FUNCTION IF EXISTS sift_flag//
CREATE FUNCTION sift_flag(p_v VARCHAR(10)) RETURNS INT DETERMINISTIC
	RETURN IF(p_v IS NULL OR p_v IN ('true', '1'), 1, 0)//

DROP PROCEDURE IF EXISTS sift_fail//
CREATE PROCEDURE sift_fail(IN p_msg VARCHAR(200))
BEGIN
	SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = p_msg;
END//

-- The options of an index as the lines the engine reads (engine.h), from the JSON of sift_attach_opts:
-- dpp, k1, b, stemmer (en|none), case_folding, accent_folding, positions, prefix_index, fields, analyzer (text|keyword),
-- merge_fanin, fresh (read the queue at query time), recheck (check the rows still exist)
DROP FUNCTION IF EXISTS sift_options//
CREATE FUNCTION sift_options(o TEXT, p_fields TEXT) RETURNS TEXT DETERMINISTIC
	RETURN CONCAT_WS(CHAR(10),
		CONCAT('dpp=', IFNULL(JSON_VALUE(o, '$.dpp'), 32)),
		CONCAT('k1=', IFNULL(JSON_VALUE(o, '$.k1'), '1.2')),
		CONCAT('b=', IFNULL(JSON_VALUE(o, '$.b'), '0.75')),
		CONCAT('stemmer=', IFNULL(JSON_VALUE(o, '$.stemmer'), '')),
		CONCAT('case_folding=', sift_flag(JSON_VALUE(o, '$.case_folding'))),
		CONCAT('accent_folding=', sift_flag(JSON_VALUE(o, '$.accent_folding'))),
		CONCAT('positions=', sift_flag(JSON_VALUE(o, '$.positions'))),
		CONCAT('prefix_index=', IFNULL(JSON_VALUE(o, '$.prefix_index'), 0)),
		CONCAT('fields=', p_fields),
		CONCAT('analyzer=', IFNULL(JSON_VALUE(o, '$.analyzer'), 'text')),
		CONCAT('merge_fanin=', IFNULL(JSON_VALUE(o, '$.merge_fanin'), 6)),
		CONCAT('fresh=', sift_flag(JSON_VALUE(o, '$.fresh'))),
		CONCAT('recheck=', sift_flag(JSON_VALUE(o, '$.recheck'))))//

DROP PROCEDURE IF EXISTS sift_attach_opts//
CREATE PROCEDURE sift_attach_opts(IN p_name VARCHAR(64), IN p_tbl VARCHAR(64), IN p_pk TEXT, IN p_text TEXT, IN p_opts TEXT)
BEGIN
	DECLARE v_db VARCHAR(64) DEFAULT DATABASE();
	DECLARE v_o TEXT DEFAULT IF(p_opts IS NULL OR p_opts = '', '{}', p_opts);
	DECLARE v_pk TEXT DEFAULT REPLACE(p_pk, ' ', '');
	DECLARE v_npk INT DEFAULT 1 + LENGTH(REPLACE(p_pk, ' ', '')) - LENGTH(REPLACE(p_pk, ',', ''));
	DECLARE v_found, v_int INT DEFAULT 0;
	DECLARE v_text, v_fields, v_keyexpr, v_source, v_from, v_cfg, v_present TEXT;
	DECLARE v_q VARCHAR(200) DEFAULT CONCAT('`', v_db, '`.`sift_q_', p_name, '`');
	DECLARE v_k VARCHAR(200) DEFAULT CONCAT('`', v_db, '`.`sift_k_', p_name, '`');
	DECLARE v_t VARCHAR(200) DEFAULT CONCAT('`', v_db, '`.`', p_tbl, '`');
	DECLARE v_newkey, v_oldkey, v_ensure, v_chg, v_textchg, v_new, v_old, v_ins TEXT;

	IF (SELECT COUNT(*) FROM sift_indexes WHERE name = p_name) > 0 THEN
		CALL sift_fail('sift: an index of that name is attached');
	END IF;
	SELECT COUNT(*), COUNT(*) = 1 AND MAX(DATA_TYPE IN ('tinyint', 'smallint', 'mediumint', 'int', 'bigint'))
		INTO v_found, v_int FROM information_schema.COLUMNS
		WHERE table_schema = v_db AND table_name = p_tbl AND FIND_IN_SET(column_name, v_pk);
	IF v_found <> v_npk THEN
		CALL sift_fail('sift: the key columns are not all columns of the table');
	END IF;
	SET v_text = p_text, v_fields = IFNULL(JSON_VALUE(v_o, '$.fields'), '');
	IF JSON_TYPE(JSON_EXTRACT(v_o, '$.fields')) = 'ARRAY' THEN
		SELECT GROUP_CONCAT(f.name ORDER BY f.n SEPARATOR ','),
			CONCAT('CONCAT(', GROUP_CONCAT(CONCAT('IFNULL((', f.expr, '), '''')') ORDER BY f.n SEPARATOR ', CHAR(29), '), ')')
			INTO v_fields, v_text
			FROM JSON_TABLE(v_o, '$.fields[*]' COLUMNS (n FOR ORDINALITY, name VARCHAR(64) PATH '$.name', expr TEXT PATH '$.expr')) f;
	END IF;
	IF v_int THEN
		SET v_keyexpr = CONCAT('t.`', v_pk, '`'), v_source = CONCAT(v_t, ' t'),
			v_from = CONCAT(v_q, ' q LEFT JOIN ', v_t, ' t ON t.`', v_pk, '` = q.pk'),
			v_newkey = CONCAT('NEW.`', v_pk, '`'), v_oldkey = CONCAT('OLD.`', v_pk, '`'), v_ensure = '';
	ELSE
		SET v_keyexpr = 'k.kid', v_source = CONCAT(v_k, ' k JOIN ', v_t, ' t ON ', sift_colcat(v_pk, 't.{c} = k.{c}', ' AND ')),
			v_from = CONCAT(v_q, ' q LEFT JOIN ', v_k, ' k ON k.kid = q.pk LEFT JOIN ', v_t, ' t ON ',
				sift_colcat(v_pk, 't.{c} = k.{c}', ' AND ')),
			v_newkey = CONCAT('(SELECT k.kid FROM ', v_k, ' k WHERE ', sift_colcat(v_pk, 'k.{c} = NEW.{c}', ' AND '), ')'),
			v_oldkey = CONCAT('(SELECT k.kid FROM ', v_k, ' k WHERE ', sift_colcat(v_pk, 'k.{c} = OLD.{c}', ' AND '), ')'),
			v_ensure = CONCAT('INSERT IGNORE INTO ', v_k, ' (', sift_colcat(v_pk, '{c}', ', '), ') VALUES (',
				sift_colcat(v_pk, 'NEW.{c}', ', '), '); ');
	END IF;
	SET v_text = REPLACE(v_text, CHAR(10), ' '), v_present = CONCAT('t.`', SUBSTRING_INDEX(v_pk, ',', 1), '` IS NOT NULL');
	SET v_cfg = CONCAT_WS(CHAR(10), sift_options(v_o, v_fields), CONCAT('queue=', v_q), CONCAT('keyexpr=', v_keyexpr),
		CONCAT('source=', v_source), CONCAT('textexpr=', v_text), CONCAT('database=', v_db), CONCAT('table=', p_tbl));
	SET v_new = REGEXP_REPLACE(v_text, '\\bt\\.', 'NEW.'), v_old = REGEXP_REPLACE(v_text, '\\bt\\.', 'OLD.');
	SET v_chg = sift_colcat(v_pk, 'NOT (NEW.{c} <=> OLD.{c})', ' OR ');
	SET v_textchg = CONCAT('NOT ((', v_new, ') <=> (', v_old, '))');
	SET v_ins = CONCAT('INSERT INTO ', v_q, ' (op, pk) VALUES ');

	SELECT sift_create(p_name, v_cfg);
	INSERT INTO sift_indexes (name, db, tbl, pkcols, keymode, qname, keyexpr, source, drain_from, present, textexpr, cfg)
		VALUES (p_name, v_db, p_tbl, v_pk, IF(v_int, 'int', 'map'), v_q, v_keyexpr, v_source, v_from, v_present, v_text, v_cfg);
	CALL sift_run(CONCAT('CREATE TABLE ', v_q, ' (seq BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY, op CHAR(1) NOT NULL, ',
		'pk BIGINT NOT NULL, ts TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP) ENGINE=InnoDB'));
	IF NOT v_int THEN
		CALL sift_run(CONCAT('CREATE TABLE ', v_k, ' (kid BIGINT NOT NULL AUTO_INCREMENT, PRIMARY KEY (kid)) ENGINE=InnoDB ',
			'SELECT ', sift_colcat(v_pk, '{c}', ', '), ' FROM ', v_t, ' LIMIT 0'));
		CALL sift_run(CONCAT('ALTER TABLE ', v_k, ' ADD UNIQUE KEY sift_uk (', sift_colcat(v_pk, '{c}', ', '), ')'));
	END IF;
	CALL sift_run(CONCAT('CREATE TRIGGER `sift_ai_', p_name, '` AFTER INSERT ON ', v_t, ' FOR EACH ROW BEGIN ',
		v_ensure, v_ins, '(''I'', ', v_newkey, '); END'));
	CALL sift_run(CONCAT('CREATE TRIGGER `sift_au_', p_name, '` AFTER UPDATE ON ', v_t, ' FOR EACH ROW BEGIN ',
		'IF ', v_chg, ' THEN ', v_ensure, v_ins, '(''D'', ', v_oldkey, '); ', v_ins, '(''I'', ', v_newkey, '); ',
		'ELSEIF ', v_textchg, ' THEN ', v_ins, '(''I'', ', v_newkey, '); END IF; END'));
	CALL sift_run(CONCAT('CREATE TRIGGER `sift_ad_', p_name, '` AFTER DELETE ON ', v_t, ' FOR EACH ROW ',
		v_ins, '(''D'', ', v_oldkey, ')'));
	IF v_int THEN
		CALL sift_run(CONCAT('INSERT INTO ', v_q, ' (op, pk) SELECT ''I'', `', v_pk, '` FROM ', v_t, ' ORDER BY `', v_pk, '`'));
	ELSE
		CALL sift_run(CONCAT('INSERT IGNORE INTO ', v_k, ' (', sift_colcat(v_pk, '{c}', ', '), ') SELECT ',
			sift_colcat(v_pk, '{c}', ', '), ' FROM ', v_t));
		CALL sift_run(CONCAT('INSERT INTO ', v_q, ' (op, pk) SELECT ''I'', kid FROM ', v_k, ' ORDER BY kid'));
	END IF;
END//

DROP PROCEDURE IF EXISTS sift_attach//
CREATE PROCEDURE sift_attach(IN p_name VARCHAR(64), IN p_tbl VARCHAR(64), IN p_pk TEXT, IN p_text TEXT, IN p_dpp INT)
BEGIN
	CALL sift_attach_opts(p_name, p_tbl, p_pk, p_text, JSON_OBJECT('dpp', p_dpp));
END//

DROP PROCEDURE IF EXISTS sift_detach//
CREATE PROCEDURE sift_detach(IN p_name VARCHAR(64))
BEGIN
	CALL sift_run(CONCAT('DROP EVENT IF EXISTS `sift_ev_', p_name, '`'));
	CALL sift_run(CONCAT('DROP TRIGGER IF EXISTS `sift_ai_', p_name, '`'));
	CALL sift_run(CONCAT('DROP TRIGGER IF EXISTS `sift_au_', p_name, '`'));
	CALL sift_run(CONCAT('DROP TRIGGER IF EXISTS `sift_ad_', p_name, '`'));
	CALL sift_run(CONCAT('DROP TABLE IF EXISTS `sift_q_', p_name, '`'));
	CALL sift_run(CONCAT('DROP TABLE IF EXISTS `sift_k_', p_name, '`'));
	DELETE FROM sift_indexes WHERE name = p_name;
	IF sift_exists(p_name) THEN
		SELECT sift_drop(p_name);
	END IF;
END//

-- A barrier row: every queue row below its seq is committed (the table lock waits for the writers in flight), so every
-- node that has the barrier in its queue has those rows too, in whatever order replication applied them. The lock request
-- also holds back the readers of the queue, so it gives up after a second (a long writing transaction): the drain then
-- uses the barrier of an earlier round and the next call tries again.
DROP PROCEDURE IF EXISTS sift_barrier//
CREATE PROCEDURE sift_barrier(IN p_q TEXT)
BEGIN
	DECLARE v_timeout INT DEFAULT @@lock_wait_timeout;
	DECLARE v_locked INT DEFAULT 1;
	DECLARE CONTINUE HANDLER FOR 1205 SET v_locked = 0;

	SET SESSION lock_wait_timeout = 1;
	CALL sift_run(CONCAT('LOCK TABLES ', p_q, ' WRITE'));
	SET SESSION lock_wait_timeout = v_timeout;
	IF v_locked THEN
		CALL sift_run(CONCAT('INSERT INTO ', p_q, ' (op, pk) VALUES (''B'', 0)'));
		CALL sift_run('UNLOCK TABLES');
	END IF;
END//

-- Drains the queue: on a node that can write (read_only is ON or OFF in this server, 1 or 0 in others: = 'OFF' is true for the writable ones), a barrier is added first when rows are waiting; the rows from the watermark to
-- the newest barrier are fed, in sequence order, to the engine, which commits one segment atomically. Nothing is deleted:
-- an interrupted drain is repeated harmlessly (the engine skips what the watermark covers).
DROP PROCEDURE IF EXISTS sift_sync//
CREATE PROCEDURE sift_sync(IN p_name VARCHAR(64))
BEGIN
	DECLARE v_q, v_from, v_text, v_present TEXT;
	DECLARE v_pruned, v_d BIGINT;
	DECLARE v_lock VARCHAR(80) DEFAULT CONCAT('sift_sync_', p_name);
	DECLARE EXIT HANDLER FOR SQLEXCEPTION
	BEGIN
		DO RELEASE_LOCK(v_lock);
		RESIGNAL;
	END;

	SELECT qname, drain_from, textexpr, present, pruned_to INTO v_q, v_from, v_text, v_present, v_pruned
		FROM sift_indexes WHERE name = p_name;
	IF v_q IS NULL THEN
		CALL sift_fail('sift: no such index');
	END IF;
	IF GET_LOCK(v_lock, 0) THEN
		SET v_d = sift_watermark(p_name);
		IF v_d < v_pruned THEN
			CALL sift_fail('sift: the queue was pruned past the watermark of this node: CALL sift_rebuild');
		END IF;
		IF @@read_only = 'OFF' THEN
			CALL sift_run(CONCAT('SELECT COUNT(*) INTO @sift_n FROM ', v_q, ' WHERE seq > ', v_d, ' AND op <> ''B'''));
			IF @sift_n > 0 THEN
				CALL sift_barrier(v_q);
			END IF;
		END IF;
		CALL sift_run(CONCAT('SELECT IFNULL(MAX(seq), 0) INTO @sift_w FROM ', v_q, ' WHERE op = ''B'''));
		SET @sift_rows = 0;
		IF @sift_w > v_d THEN
			CALL sift_run(CONCAT('SELECT sift_drain(''', p_name, ''', q.seq, q.op, q.pk, IF(', v_present,
				', CONVERT(', v_text, ' USING utf8mb4), NULL)) INTO @sift_rows FROM ', v_from, ' WHERE q.seq > ', v_d, ' AND q.seq <= @sift_w'));
		END IF;
		DO RELEASE_LOCK(v_lock);
		SELECT @sift_rows AS rows_applied;
	END IF;
END//

-- Deletes the drained queue rows older than p_seconds. The rows are the log the other nodes replay, so this is the
-- retention: a node that is further behind than that has to CALL sift_rebuild.
DROP PROCEDURE IF EXISTS sift_prune//
CREATE PROCEDURE sift_prune(IN p_name VARCHAR(64), IN p_seconds INT)
BEGIN
	DECLARE v_q TEXT;
	DECLARE v_d BIGINT DEFAULT sift_watermark(p_name);

	SELECT qname INTO v_q FROM sift_indexes WHERE name = p_name;
	IF v_q IS NULL THEN
		CALL sift_fail('sift: no such index');
	END IF;
	START TRANSACTION;
	CALL sift_run(CONCAT('SELECT IFNULL(MAX(seq), 0) INTO @sift_m FROM ', v_q, ' WHERE seq < ', v_d,
		' AND ts < NOW() - INTERVAL ', p_seconds, ' SECOND'));
	IF @sift_m > 0 THEN
		CALL sift_run(CONCAT('DELETE FROM ', v_q, ' WHERE seq <= ', @sift_m));
		UPDATE sift_indexes SET pruned_to = GREATEST(pruned_to, @sift_m) WHERE name = p_name;
	END IF;
	COMMIT;
	SELECT @sift_m AS pruned_to;
END//

-- Builds the index of this node from the table: after a restore, on a replica, or when the queue was pruned past the node.
-- The table is read in one consistent snapshot together with the newest barrier, so what the snapshot lacks comes with
-- the queue rows after the barrier.
DROP PROCEDURE IF EXISTS sift_rebuild//
CREATE PROCEDURE sift_rebuild(IN p_name VARCHAR(64))
BEGIN
	DECLARE v_q, v_source, v_text, v_key, v_cfg TEXT;
	DECLARE v_lock VARCHAR(80) DEFAULT CONCAT('sift_sync_', p_name);
	DECLARE EXIT HANDLER FOR SQLEXCEPTION
	BEGIN
		ROLLBACK;
		DO RELEASE_LOCK(v_lock);
		RESIGNAL;
	END;

	SELECT qname, source, textexpr, keyexpr, cfg INTO v_q, v_source, v_text, v_key, v_cfg FROM sift_indexes WHERE name = p_name;
	IF v_q IS NULL THEN
		CALL sift_fail('sift: no such index');
	END IF;
	IF NOT sift_exists(p_name) THEN
		SELECT sift_create(p_name, v_cfg);
	END IF;
	IF GET_LOCK(v_lock, 60) THEN
		IF @@read_only = 'OFF' THEN
			CALL sift_barrier(v_q);
		END IF;
		START TRANSACTION WITH CONSISTENT SNAPSHOT;
		CALL sift_run(CONCAT('SELECT IFNULL(MAX(seq), 0) INTO @sift_w FROM ', v_q, ' WHERE op = ''B'''));
		CALL sift_run(CONCAT('SELECT sift_load(''', p_name, ''', @sift_w, ', v_key, ', CONVERT(', v_text, ' USING utf8mb4)) INTO @sift_rows FROM ',
			v_source));
		COMMIT;
		DO RELEASE_LOCK(v_lock);
		SELECT @sift_rows AS rows_loaded, @sift_w AS watermark;
	END IF;
END//

-- The filter of a SQL predicate over the table (alias t): the keys of the rows that satisfy it, for sift_topk and friends
DROP PROCEDURE IF EXISTS sift_filter//
CREATE PROCEDURE sift_filter(IN p_name VARCHAR(64), IN p_where TEXT)
BEGIN
	DECLARE v_key, v_source TEXT;

	SELECT keyexpr, source INTO v_key, v_source FROM sift_indexes WHERE name = p_name;
	IF v_key IS NULL THEN
		CALL sift_fail('sift: no such index');
	END IF;
	CALL sift_run(CONCAT('SELECT sift_idset(', v_key, ') INTO @sift_f FROM ', v_source, ' WHERE ', p_where));
END//

-- The top p_k rows of the table for a query, best first, with their score; p_where (alias t) restricts the rows
-- before they are scored, p_where NULL does not
DROP PROCEDURE IF EXISTS sift_search//
CREATE PROCEDURE sift_search(IN p_name VARCHAR(64), IN p_query TEXT, IN p_k INT, IN p_where TEXT)
BEGIN
	DECLARE v_key, v_source TEXT;

	SELECT keyexpr, source INTO v_key, v_source FROM sift_indexes WHERE name = p_name;
	IF v_key IS NULL THEN
		CALL sift_fail('sift: no such index');
	END IF;
	IF p_where IS NOT NULL THEN
		CALL sift_filter(p_name, p_where);
	END IF;
	SET @sift_p = p_query;
	CALL sift_run(CONCAT('SELECT t.*, j.s AS score FROM (SELECT sift_topk(''', p_name, ''', @sift_p, ', p_k,
		IF(p_where IS NULL, '', ', @sift_f'), ') AS r) x, JSON_TABLE(x.r, ''$[*]'' COLUMNS (n FOR ORDINALITY, id BIGINT PATH ''$.id'', ',
		's DOUBLE PATH ''$.s'')) j JOIN ', v_source, ' ON ', v_key, ' = j.id ORDER BY j.n'));
END//

-- Counts of the matches of a query per value of an expression over the table (alias t), most frequent first
DROP PROCEDURE IF EXISTS sift_facets//
CREATE PROCEDURE sift_facets(IN p_name VARCHAR(64), IN p_query TEXT, IN p_expr TEXT, IN p_where TEXT)
BEGIN
	DECLARE v_key, v_source TEXT;

	SELECT keyexpr, source INTO v_key, v_source FROM sift_indexes WHERE name = p_name;
	IF v_key IS NULL THEN
		CALL sift_fail('sift: no such index');
	END IF;
	IF p_where IS NOT NULL THEN
		CALL sift_filter(p_name, p_where);
	END IF;
	SET @sift_p = p_query;
	CALL sift_run(CONCAT('SELECT ', p_expr, ' AS value, COUNT(*) AS n FROM (SELECT sift_matches(''', p_name, ''', @sift_p',
		IF(p_where IS NULL, '', ', @sift_f'), ') AS r) x, JSON_TABLE(x.r, ''$[*]'' COLUMNS (id BIGINT PATH ''$'')) j JOIN ', v_source, ' ON ', v_key,
		' = j.id GROUP BY value ORDER BY n DESC, value'));
END//

-- Runs sift_sync and sift_prune every p_seconds in the event scheduler (SET GLOBAL event_scheduler = ON)
DROP PROCEDURE IF EXISTS sift_schedule//
CREATE PROCEDURE sift_schedule(IN p_name VARCHAR(64), IN p_seconds INT)
BEGIN
	CALL sift_run(CONCAT('CREATE OR REPLACE EVENT `sift_ev_', p_name, '` ON SCHEDULE EVERY ', p_seconds,
		' SECOND DO BEGIN CALL sift_sync(''', p_name, '''); CALL sift_prune(''', p_name, ''', 3600); END'));
END//

DELIMITER ;
