-- Queue, triggers and drain of a sift index (load after functions.sql, in the database of the indexed table).
--
--   CALL sift_attach('wiki', 'docs', 'id', 't.body', 32);   -- index docs.body: creates the engine index, the queue
--                                                            -- table sift_q_wiki, the three triggers, queues the rows
--   CALL sift_sync('wiki');                                  -- drains the queue into a new segment
--   SELECT d.id, j.s FROM JSON_TABLE(sift_topk('wiki', 'a AND b', 10), '$[*]'
--          COLUMNS (n FOR ORDINALITY, id BIGINT PATH '$.id', s DOUBLE PATH '$.s')) j JOIN docs d ON d.id = j.id ORDER BY j.n;
--   CALL sift_detach('wiki');
--
-- The text expression is written over the alias t of the indexed table (t.body, CONCAT_WS(' ', t.title, t.body)); the
-- primary key is an integer. The triggers only queue (op, pk) in the writer's transaction: a rolled-back write leaves no
-- trace. A drain reads the committed rows of the queue and their current text, so it indexes the latest version of a row.
DELIMITER //

CREATE TABLE IF NOT EXISTS sift_indexes (
	name     VARCHAR(64) NOT NULL PRIMARY KEY,
	tbl      VARCHAR(64) NOT NULL,
	pkcol    VARCHAR(64) NOT NULL,
	textexpr TEXT NOT NULL
) ENGINE=InnoDB//

DROP PROCEDURE IF EXISTS sift_run//
CREATE PROCEDURE sift_run(IN sql_text TEXT)
BEGIN
	SET @sift_sql = sql_text;
	PREPARE sift_st FROM @sift_sql;
	EXECUTE sift_st;
	DEALLOCATE PREPARE sift_st;
END//

DROP PROCEDURE IF EXISTS sift_attach//
CREATE PROCEDURE sift_attach(IN p_name VARCHAR(64), IN p_tbl VARCHAR(64), IN p_pk VARCHAR(64), IN p_text TEXT, IN p_dpp INT)
BEGIN
	DECLARE v_new TEXT DEFAULT REPLACE(p_text, 't.', 'NEW.');
	DECLARE v_old TEXT DEFAULT REPLACE(p_text, 't.', 'OLD.');
	DECLARE v_ins TEXT DEFAULT CONCAT('INSERT INTO `sift_q_', p_name, '` (op, pk) VALUES ');

	SELECT sift_create(p_name, p_dpp);
	INSERT INTO sift_indexes VALUES (p_name, p_tbl, p_pk, p_text);
	CALL sift_run(CONCAT('CREATE TABLE `sift_q_', p_name, '` (seq BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY, ',
		'op CHAR(1) NOT NULL, pk BIGINT NOT NULL) ENGINE=InnoDB'));
	CALL sift_run(CONCAT('CREATE TRIGGER `sift_ai_', p_name, '` AFTER INSERT ON `', p_tbl, '` FOR EACH ROW ',
		v_ins, '(''I'', NEW.`', p_pk, '`)'));
	CALL sift_run(CONCAT('CREATE TRIGGER `sift_au_', p_name, '` AFTER UPDATE ON `', p_tbl, '` FOR EACH ROW BEGIN ',
		'IF NOT (NEW.`', p_pk, '` <=> OLD.`', p_pk, '`) THEN ', v_ins, '(''D'', OLD.`', p_pk, '`); END IF; ',
		'IF NOT (NEW.`', p_pk, '` <=> OLD.`', p_pk, '`) OR NOT ((', v_new, ') <=> (', v_old, ')) THEN ',
		v_ins, '(''I'', NEW.`', p_pk, '`); END IF; END'));
	CALL sift_run(CONCAT('CREATE TRIGGER `sift_ad_', p_name, '` AFTER DELETE ON `', p_tbl, '` FOR EACH ROW ',
		v_ins, '(''D'', OLD.`', p_pk, '`)'));
	CALL sift_run(CONCAT('INSERT INTO `sift_q_', p_name, '` (op, pk) SELECT ''I'', `', p_pk, '` FROM `', p_tbl, '` ORDER BY `', p_pk, '`'));
END//

DROP PROCEDURE IF EXISTS sift_detach//
CREATE PROCEDURE sift_detach(IN p_name VARCHAR(64))
BEGIN
	CALL sift_run(CONCAT('DROP TRIGGER IF EXISTS `sift_ai_', p_name, '`'));
	CALL sift_run(CONCAT('DROP TRIGGER IF EXISTS `sift_au_', p_name, '`'));
	CALL sift_run(CONCAT('DROP TRIGGER IF EXISTS `sift_ad_', p_name, '`'));
	CALL sift_run(CONCAT('DROP TABLE IF EXISTS `sift_q_', p_name, '`'));
	DELETE FROM sift_indexes WHERE name = p_name;
	SELECT sift_drop(p_name);
END//

-- Drains the queue: a table lock (taken and released at once) waits for the writers in flight, so every row up to the
-- highest sequence number is committed; the aggregate then feeds the rows, in sequence order, to the engine, which commits
-- one segment atomically; the drained rows are deleted last (a drain interrupted before that is repeated harmlessly).
DROP PROCEDURE IF EXISTS sift_sync//
CREATE PROCEDURE sift_sync(IN p_name VARCHAR(64))
BEGIN
	DECLARE v_tbl, v_pk VARCHAR(64);
	DECLARE v_text TEXT;
	DECLARE v_q VARCHAR(80) DEFAULT CONCAT('`sift_q_', p_name, '`');

	SELECT tbl, pkcol, textexpr INTO v_tbl, v_pk, v_text FROM sift_indexes WHERE name = p_name;
	IF GET_LOCK(CONCAT('sift_sync_', p_name), 0) THEN
		CALL sift_run(CONCAT('LOCK TABLES ', v_q, ' WRITE'));
		CALL sift_run(CONCAT('SELECT IFNULL(MAX(seq), 0) INTO @sift_w FROM ', v_q));
		CALL sift_run('UNLOCK TABLES');
		SET @sift_rows = 0;
		CALL sift_run(CONCAT('SELECT sift_drain(''', p_name, ''', q.seq, q.op, q.pk, ', v_text, ') INTO @sift_rows FROM ', v_q,
			' q LEFT JOIN `', v_tbl, '` t ON t.`', v_pk, '` = q.pk WHERE q.seq <= @sift_w'));
		CALL sift_run(CONCAT('DELETE FROM ', v_q, ' WHERE seq <= @sift_w'));
		DO RELEASE_LOCK(CONCAT('sift_sync_', p_name));
		SELECT @sift_rows AS rows_applied;
	END IF;
END//

DELIMITER ;
