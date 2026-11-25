/*
 * tests/cfg_check.c - the definitions cfg_check.c accepts (the forms sift_attach writes) and the injection payloads it refuses.
 * A program of its own: no server, no engine.
 */
#include <stdio.h>
#include <string.h>

#include "cfg_check.h"

static int	failed;

typedef struct Case
{
	const char *label;
	const char *name;
	const char *key;			/* the key of the definition replaced by value, "" for none */
	const char *value;
	bool		ok;
} Case;

#define BASE_INT	"queue=`db`.`sift_q_ix`\nkeyexpr=t.`id`\nsource=`db`.`docs` t\ntextexpr=t.body\ndatabase=db\ntable=docs\nk1=1.2"
#define BASE_MAP	"queue=`db`.`sift_q_ix`\nkeyexpr=k.kid\nsource=`db`.`sift_k_ix` k JOIN `db`.`docs` t ON t.`a` = k.`a` AND t.`b` = k.`b`\n" \
					"textexpr=t.body\ndatabase=db\ntable=docs"

static const char *
with(const char *base, const char *key, const char *value, char *out, size_t n)
{
	char		line[1024];
	char	   *at;

	if (key[0] == '\0')
		return base;
	snprintf(line, sizeof(line), "\n%s=", key);
	snprintf(out, n, "\n%s", base);
	if ((at = strstr(out, line)) == NULL)
		return out + 1;
	{
		const char *rest = strchr(at + 1, '\n');
		char		tail[1024];

		snprintf(tail, sizeof(tail), "%s", rest ? rest : "");
		snprintf(at + 1, n - (at + 1 - out), "%s=%s%s", key, value, tail);
	}
	return out + 1;
}

static void
run(const char *base, const Case *c)
{
	char		cfg[4096],
				err[256] = "";
	bool		got = cfg_check(c->name, with(base, c->key, c->value, cfg, sizeof(cfg)), err, sizeof(err));

	if (got != c->ok)
	{
		failed++;
		fprintf(stderr, "FAIL %s: expected %s, got %s (%s)\n", c->label, c->ok ? "accepted" : "refused", got ? "accepted" : "refused", err);
	}
	else if (!got && strncmp(err, "sift:", 5) != 0)
	{
		failed++;
		fprintf(stderr, "FAIL %s: the refusal says [%s]\n", c->label, err);
	}
}

static const Case int_cases[] = {
	{"as sift_attach writes it", "ix", "", "", true},
	{"a plain text column", "ix", "textexpr", "t.title", true},
	{"concat_ws with a literal", "ix", "textexpr", "CONCAT_WS(' ', t.title, t.body)", true},
	{"the fields form", "ix", "textexpr", "CONCAT(IFNULL((t.title), ''), CHAR(29), IFNULL((t.body), ''))", true},
	{"a quoted column", "ix", "textexpr", "t.`my body`", true},
	{"a cast", "ix", "textexpr", "CAST(t.n AS CHAR)", true},
	{"a conversion", "ix", "textexpr", "CONVERT(t.body USING utf8mb4)", true},
	{"a case", "ix", "textexpr", "CASE WHEN t.a IS NULL THEN '' ELSE t.a END", true},
	{"an escaped quote", "ix", "textexpr", "CONCAT(t.body, 'it''s')", true},
	{"no definition keys at all", "ix", "", "", true},
	{"a sub-select", "ix", "textexpr", "(SELECT Priv FROM mysql.global_priv LIMIT 1)", false},
	{"a UNION", "ix", "textexpr", "t.body UNION SELECT Priv FROM mysql.global_priv", false},
	{"a statement separator", "ix", "textexpr", "t.body; DROP TABLE x", false},
	{"a line comment", "ix", "textexpr", "t.body -- x", false},
	{"a block comment", "ix", "textexpr", "t.body /* x */", false},
	{"a hash comment", "ix", "textexpr", "t.body # x", false},
	{"a system variable", "ix", "textexpr", "@@version", false},
	{"a user variable", "ix", "textexpr", "@x", false},
	{"load_file", "ix", "textexpr", "LOAD_FILE('/etc/passwd')", false},
	{"sleep", "ix", "textexpr", "CONCAT(t.body, SLEEP(5))", false},
	{"benchmark", "ix", "textexpr", "BENCHMARK(100000000, MD5('x'))", false},
	{"a sift function", "ix", "textexpr", "sift_drop('ix')", false},
	{"a function with a space before the parenthesis", "ix", "textexpr", "sleep (5)", false},
	{"a bare table", "ix", "textexpr", "mysql.user", false},
	{"another alias", "ix", "textexpr", "u.Priv", false},
	{"a backslash in a literal", "ix", "textexpr", "CONCAT(t.body, '\\')", false},
	{"an unterminated literal", "ix", "textexpr", "CONCAT(t.body, 'x)", false},
	{"an unbalanced parenthesis", "ix", "textexpr", "CONCAT(t.body", false},
	{"a closing parenthesis first", "ix", "textexpr", ") FROM mysql.user WHERE (1", false},
	{"a double quoted string", "ix", "textexpr", "\"x\"", false},
	{"a column that opens a quote", "ix", "textexpr", "t.`x` FROM mysql.user -- `", false},
	{"an empty expression", "ix", "textexpr", "", false},
	{"a source with another schema", "ix", "source", "mysql.global_priv", false},
	{"a source with a second table", "ix", "source", "`db`.`docs` t, mysql.global_priv g", false},
	{"a source with a join", "ix", "source", "`db`.`docs` t JOIN mysql.user u ON 1=1", false},
	{"a source of another database", "ix", "source", "`mysql`.`docs` t", false},
	{"a source of another table", "ix", "source", "`db`.`other` t", false},
	{"a queue that is a sub-select", "ix", "queue", "(SELECT 1 seq, 'I' op, 1 pk) q", false},
	{"a queue of another schema", "ix", "queue", "mysql.global_priv", false},
	{"a queue that is not named after the index", "ix", "queue", "`db`.`victim`", false},
	{"a queue of another index", "ix", "queue", "`db`.`sift_q_other`", false},
	{"a constant key", "ix", "keyexpr", "1", false},
	{"a key that is a sub-select", "ix", "keyexpr", "(SELECT COUNT(*) FROM mysql.user)", false},
	{"a key with trailing SQL", "ix", "keyexpr", "t.`id` OR 1=1", false},
	{"a key with a backtick escape", "ix", "keyexpr", "t.`id`` FROM mysql.user -- `", false},
	{"a database that is not the one of the queue", "ix", "database", "mysql", false},
	{"a database with a backtick", "ix", "database", "d`b", false},
	{"a table with a backtick", "ix", "table", "docs`", false},
	{"an index name with a quote", "i'x", "", "", false},
	{"an index name with a backtick", "i`x", "", "", false},
	{"an empty index name", "", "", "", false},
};

static const Case map_cases[] = {
	{"a key table as sift_attach writes it", "ix", "", "", true},
	{"a join on another column", "ix", "source",
	 "`db`.`sift_k_ix` k JOIN `db`.`docs` t ON t.`a` = k.`z`", false},
	{"a join with a trailing condition", "ix", "source",
	 "`db`.`sift_k_ix` k JOIN `db`.`docs` t ON t.`a` = k.`a` AND t.`b` = k.`b` OR 1=1", false},
	{"a join with a second table", "ix", "source",
	 "`db`.`sift_k_ix` k JOIN `db`.`docs` t ON t.`a` = k.`a` AND t.`b` = k.`b`, mysql.user u", false},
	{"a key table of another index", "ix", "source",
	 "`db`.`sift_k_other` k JOIN `db`.`docs` t ON t.`a` = k.`a` AND t.`b` = k.`b`", false},
};

int
main(void)
{
	for (size_t i = 0; i < sizeof(int_cases) / sizeof(*int_cases); i++)
		run(BASE_INT, &int_cases[i]);
	for (size_t i = 0; i < sizeof(map_cases) / sizeof(*map_cases); i++)
		run(BASE_MAP, &map_cases[i]);
	{
		char		err[256];
		const Case	none = {"no SQL keys", "ix", "", "", true};

		if (!cfg_check("ix", "k1=1.2\nb=0.75", err, sizeof(err)) || !none.ok)
		{
			failed++;
			fprintf(stderr, "FAIL a definition with options only is refused: %s\n", err);
		}
		if (cfg_check("ix", "queue=`db`.`sift_q_ix`\nk1=1.2", err, sizeof(err)))
		{
			failed++;
			fprintf(stderr, "FAIL a definition with one of the SQL keys is accepted\n");
		}
	}
	printf("cfg_check_test: %s\n", failed ? "FAILED" : "ok");
	return failed != 0;
}
