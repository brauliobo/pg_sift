#!/usr/bin/env bash
# Privilege and injection tests: the UDFs read the queue and the table through a local connection of the server (no ACL
# of its own), so an index definition (the cfg of sift_create) must never let a user run SQL of their choosing there, and
# the procedures of sift.sql must neither concatenate names into SQL nor run with the privileges of their definer.
#   mariadb/tests/security.sh
set -uo pipefail
. "$(dirname "$0")/lib.sh"
NAME=security
DB=sec
USR=sift_unpriv
APP=sift_app

usql() { mariadb --no-defaults -u"$USR" -ppw -S "$SIFT_MDB_ROOT/mysql.sock" -N -B "$@" 2>&1 | firsterr; }
asql() { mariadb --no-defaults -u"$APP" -ppw -S "$SIFT_MDB_ROOT/mysql.sock" -N -B "$DB" -e "$1" 2>&1 | firsterr; }
nl=$'\n'

fresh_db ctl inj pwn app1 sq
sql "DROP USER IF EXISTS '$USR'@'localhost'; DROP USER IF EXISTS '$APP'@'localhost';
  CREATE USER '$USR'@'localhost' IDENTIFIED BY 'pw'; GRANT USAGE ON *.* TO '$USR'@'localhost';
  CREATE USER '$APP'@'localhost' IDENTIFIED BY 'pw'; GRANT EXECUTE ON $DB.* TO '$APP'@'localhost'"
sql "CREATE TABLE docs (id BIGINT PRIMARY KEY, body TEXT); INSERT INTO docs VALUES (1, 'alpha beta'), (2, 'gamma')"
sql "CREATE TABLE victim (id INT PRIMARY KEY); INSERT INTO victim VALUES (1)"
sql "CREATE TABLE sift_q_ctl (seq BIGINT AUTO_INCREMENT PRIMARY KEY, op CHAR(1), pk BIGINT, ts TIMESTAMP DEFAULT CURRENT_TIMESTAMP)"
sql "CREATE TABLE sift_q_inj (seq BIGINT AUTO_INCREMENT PRIMARY KEY, op CHAR(1), pk BIGINT, ts TIMESTAMP DEFAULT CURRENT_TIMESTAMP)"

cfg() {   # name [key=value ...]: a definition as sift_attach writes it, with the given keys replaced
  local name=$1 k; shift
  declare -A kv=([queue]="\`$DB\`.\`sift_q_$name\`" [keyexpr]='t.`id`' [source]="\`$DB\`.\`docs\` t" [textexpr]='t.body'
    [database]=$DB [table]=docs [k1]=1.2)
  for k in "$@"; do kv[${k%%=*}]=${k#*=}; done
  printf '%s\n' "queue=${kv[queue]}" "keyexpr=${kv[keyexpr]}" "source=${kv[source]}" "textexpr=${kv[textexpr]}" \
    "database=${kv[database]}" "table=${kv[table]}" "k1=${kv[k1]}"
}
quote() { printf "%s" "${1//\'/\'\'}"; }
create_as_unpriv() { usql -e "SELECT sift_create('$1', '$(quote "$(cfg "$@")")')"; }

check_match "a definition written the way sift_attach writes it is accepted" "^created ctl$" "$(create_as_unpriv ctl)"
sql "SELECT sift_drop('ctl')" >/dev/null

# every definition below runs SQL of the creator's choosing as the server itself if the library takes it
reject() {   # label name key=value ...
  local label=$1 name=$2; shift 2
  local out; out=$(create_as_unpriv "$name" "$@")
  check_match "cfg: $label is refused" "^ERROR .*sift:" "$out"
  check "cfg: $label leaves no index" 0 "$(sql "SELECT sift_exists('$name')")"
  sql "SELECT sift_drop('$name')" >/dev/null 2>&1
}
reject "a source that reads another schema" inj "source=mysql.global_priv"
reject "a source with a second table" inj "source=\`$DB\`.\`docs\` t, mysql.global_priv g"
reject "a source with a join on the privilege tables" inj "source=\`$DB\`.\`docs\` t JOIN mysql.user u ON 1=1"
reject "a queue that is a subquery" inj "queue=(SELECT 1 seq, 'I' op, 1 pk) q"
reject "a queue of another schema" inj "queue=mysql.global_priv"
reject "a queue that is not named after the index" inj "queue=\`$DB\`.\`victim\`"
reject "a key expression with a subquery" inj "keyexpr=(SELECT COUNT(*) FROM mysql.user)"
reject "a key expression that is a constant" inj "keyexpr=1"
reject "a text expression with a subquery" inj "textexpr=(SELECT Priv FROM mysql.global_priv LIMIT 1)"
reject "a text expression with a UNION" inj "textexpr=t.body UNION SELECT Priv FROM mysql.global_priv"
reject "a text expression that closes the call" inj "textexpr=t.body) FROM mysql.global_priv -- "
reject "a text expression that reads a file" inj "textexpr=LOAD_FILE('/etc/passwd')"
reject "a text expression that sleeps" inj "textexpr=CONCAT(t.body, SLEEP(5))"
reject "a text expression that calls a sift function" inj "textexpr=sift_drop('victim')"
reject "a text expression with a statement separator" inj "textexpr=t.body; DROP TABLE victim"
reject "a text expression with a comment" inj "textexpr=t.body /* x */"
reject "a text expression with a system variable" inj "textexpr=@@version"
reject "a text expression with a backslash escape" inj "textexpr=CONCAT(t.body, '\\\\')"
reject "a database that is not the one of the queue" inj "database=mysql"
reject "a table that carries a backtick" inj "table=docs\`"
check "victim survives the definitions" 1 "$(sql "SELECT COUNT(*) FROM victim")"

create_as_unpriv pwn "queue=(SELECT 1 seq, 'I' op, 1 pk) q" "keyexpr=1" "source=mysql.global_priv" "textexpr=CONCAT(Host, ':', User, ':', Priv)" >/dev/null
check "an index over the privilege tables answers nothing" "" "$(usql -e "SELECT sift_count('pwn', 'mysql_native_password OR authentication_string')" | grep -E '^[1-9]')"

# names that reach SQL as identifiers or as string literals: refused before any statement is built
sql "CREATE USER IF NOT EXISTS '$APP'@'localhost' IDENTIFIED BY 'pw'"
check_match "sift_detach refuses a name that closes the identifier" "^ERROR .*sift:" "$(asql "CALL sift_detach('z\`, \`victim')")"
check "sift_detach did not drop the victim table" 1 "$(sql "SELECT COUNT(*) FROM information_schema.TABLES WHERE table_schema = '$DB' AND table_name = 'victim'")"
check_match "sift_schedule refuses a name that closes the literal" "^ERROR .*sift:" "$(asql "CALL sift_schedule('a''); DROP TABLE victim; CALL sift_sync(''b', 3600)")"
check "sift_schedule created no event" 0 "$(sql "SELECT COUNT(*) FROM information_schema.EVENTS WHERE event_schema = '$DB'")"
check_match "sift_attach refuses a name with a quote" "^ERROR .*sift:" "$(asql "CALL sift_attach('a\`b', 'docs', 'id', 't.body', 8)")"
check_match "sift_attach refuses a text expression with a subquery" "^ERROR .*sift:" \
  "$(sqlerr "CALL sift_attach('sq', 'docs', 'id', '(SELECT Priv FROM mysql.global_priv LIMIT 1)', 8)")"
check "a refused sift_attach leaves no queue table" 0 "$(sql "SELECT COUNT(*) FROM information_schema.TABLES WHERE table_schema = '$DB' AND table_name = 'sift_q_sq'")"
check "a refused sift_attach leaves no index" 0 "$(sql "SELECT sift_exists('sq')")"

# the procedures run with the privileges of the caller: a caller without privileges on the table cannot attach
check_match "sift_attach needs the privileges of the caller" "^ERROR" "$(asql "CALL sift_attach('app1', 'docs', 'id', 't.body', 8)")"
check "no routine runs as its definer" 0 "$(sql "SELECT COUNT(*) FROM information_schema.ROUTINES WHERE routine_schema = '$DB' AND security_type = 'DEFINER'")"

sql "DROP USER IF EXISTS '$USR'@'localhost'; DROP USER IF EXISTS '$APP'@'localhost'"
for i in pwn app1; do sql "SELECT sift_drop('$i')" >/dev/null 2>&1; done
finish
