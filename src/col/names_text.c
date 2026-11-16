/*
 * col/names_text.c - the options of a sift_col index that name a table or a column as text, written again when the name changes (col/names.c has the event, this file has the text)
 *
 * pg_dump writes the options as they are and a restore resolves them again, so a name left behind loses the index. Four kinds of text name a column of the table of the index:
 *   - the lists (key_cut, key_columns, rollup_columns, ..): a name between separators; a few of them have words of their own (order_layout: compact, whole; compression: the codecs);
 *   - the heads of paths (paths, rollup_leaves): the column that a path or the leaf of an array starts with (doc.k, doc->k, items[].kind, tags#1);
 *   - connect and scope: the column of the index that holds the key, the columns of the other table, the table with its schema, the btree a scope uses. They are parsed by the parsers
 *     of the options (col/conn_decl.c, col/scope_parse.c), changed as specs and written again, so no name is ever found by looking at text.
 * An unqualified table of connect or scope is in the schema of the table of the index: the table of the index that moves to another schema keeps the schema its names had.
 */
#include "postgres.h"

#include "access/reloptions.h"
#include "catalog/pg_class.h"
#include "commands/defrem.h"
#include "nodes/makefuncs.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "col/am.h"
#include "col/conn.h"
#include "col/names.h"
#include "col/scope.h"
#include "core/catname.h"

typedef enum NamesMode
{
	NAMES_PLAIN,				/* every name between the separators */
	NAMES_LAYOUT,				/* ... but the layouts: 'compact: a, b; whole: c' */
	NAMES_COMPRESSION,			/* only the names with a codec: 'auto; a=zstd:9' */
	NAMES_PATHS,				/* the head of each entry: 'doc.k int4, +tags[] int4' */
	NAMES_LEAVES				/* the head of each leaf: 'items[].kind, tags#1' */
} NamesMode;

static const struct
{
	const char *option;
	NamesMode	mode;
}			list_options[] = {
	{"key_cut", NAMES_PLAIN}, {"key_columns", NAMES_PLAIN}, {"rollup_columns", NAMES_PLAIN}, {"rollup_dims", NAMES_PLAIN}, {"calendar_columns", NAMES_PLAIN},
	{"count_columns", NAMES_PLAIN}, {"point_columns", NAMES_PLAIN}, {"point_tid_columns", NAMES_PLAIN}, {"order_columns", NAMES_PLAIN}, {"order_array_columns", NAMES_PLAIN},
	{"order_array_group", NAMES_PLAIN}, {"key_order", NAMES_PLAIN}, {"hll_columns", NAMES_PLAIN}, {"return_columns", NAMES_PLAIN}, {"track_changes", NAMES_PLAIN},
	{"preagg", NAMES_PLAIN}, {"order_layout", NAMES_LAYOUT}, {"compression", NAMES_COMPRESSION}, {"paths", NAMES_PATHS}, {"rollup_leaves", NAMES_LEAVES},
	{"rollup", NAMES_LEAVES}
};

/* Words of the lists that are not a column's name */
static const char *const option_words[] = {"auto", "off", "on", "all", "ctid", "cube", "none", "default"};

static bool
is_word(const char *tok, size_t len)
{
	for (int i = 0; i < lengthof(option_words); i++)
		if (strlen(option_words[i]) == len && strncmp(option_words[i], tok, len) == 0)
			return true;
	return false;
}

/* The name at p (a double-quoted one or a run up to a stop), its end; *name is the name unquoted */
static const char *
read_head(const char *p, const char *stops, char **name)
{
	StringInfoData b;

	initStringInfo(&b);
	if (*p == '"')
	{
		for (p++; *p != '\0'; p++)
		{
			if (*p == '"' && p[1] == '"')
				p++;
			else if (*p == '"')
			{
				p++;
				break;
			}
			appendStringInfoChar(&b, *p);
		}
	}
	else
	{
		size_t		len = strcspn(p, stops);

		appendBinaryStringInfo(&b, p, len);
		p += len;
	}
	*name = b.data;
	return p;
}

/* The text of a list with the names that are `from` written as `to`, NULL when it has none (a name is a run between the separators of the lists) */
static char *
rewrite_names(const char *text, NamesMode mode, const char *from, const char *to)
{
	StringInfoData out;
	size_t		flen = strlen(from);
	bool		changed = false;

	initStringInfo(&out);
	for (const char *p = text; *p != '\0';)
	{
		size_t		len = strcspn(p, ", \t:;()=");

		if (len == 0)
			appendStringInfoChar(&out, *p++);
		else
		{
			const char *next = p + len + strspn(p + len, " \t");
			bool		hit = len == flen && strncmp(p, from, len) == 0 && !is_word(p, len);

			if ((mode == NAMES_LAYOUT && *next == ':') || (mode == NAMES_COMPRESSION && *next != '='))
				hit = false;
			if (hit)
				appendStringInfoString(&out, quote_identifier(to));
			else
				appendBinaryStringInfo(&out, p, len);
			changed |= hit;
			p += len;
		}
	}
	return changed ? out.data : NULL;
}

/* The text of a list of paths or leaves with the heads that are `from` written as `to`: an entry starts after the start, a ',' or a ';' outside braces, and after 'auto:' and '+' */
static char *
rewrite_heads(const char *text, NamesMode mode, const char *from, const char *to)
{
	const char *stops = mode == NAMES_PATHS ? ".-[ ,;{}:" : ".-[# ,;{}:";
	StringInfoData out;
	int			depth = 0;
	bool		start = true,
				changed = false;

	initStringInfo(&out);
	for (const char *p = text; *p != '\0';)
	{
		if (depth == 0 && start && (*p == ' ' || *p == '+'))
			appendStringInfoChar(&out, *p++);
		else if (depth == 0 && start && strncmp(p, "auto:", 5) == 0)
		{
			appendBinaryStringInfo(&out, p, 5);
			p += 5;
		}
		else if (depth == 0 && start)
		{
			char	   *name;
			const char *end = read_head(p, stops, &name);
			bool		hit = strcmp(name, from) == 0 && !is_word(name, strlen(name));

			if (hit)
				appendStringInfoString(&out, quote_identifier(to));
			else
				appendBinaryStringInfo(&out, p, end - p);
			changed |= hit;
			p = end;
			start = false;
		}
		else
		{
			depth += (*p == '{') - (*p == '}');
			start = depth == 0 && (*p == ',' || *p == ';');
			appendStringInfoChar(&out, *p++);
		}
	}
	return changed ? out.data : NULL;
}

char *
sift_names_list_option(const char *option, const char *text, const char *from, const char *to)
{
	for (int i = 0; i < lengthof(list_options); i++)
		if (strcmp(list_options[i].option, option) == 0)
			return list_options[i].mode == NAMES_PATHS || list_options[i].mode == NAMES_LEAVES ? rewrite_heads(text, list_options[i].mode, from, to) :
				rewrite_names(text, list_options[i].mode, from, to);
	return NULL;
}

/* A column of the table of the index (the host) named in the text */
static bool
follow_host(const SiftNameChange *c, Oid host, char **col)
{
	if (c->kind != SIFT_NAME_COLUMN || host != c->relid || strcmp(*col, c->from) != 0)
		return false;
	*col = pstrdup(c->to);
	return true;
}

/*
 * A relation named by schema and name (with the columns, and the btree, the text says of it) in the options of an index of the table host. A column renamed or an index renamed: the
 * name resolves to the table as the catalog has it now. A table renamed or moved: the name it had is compared with the name the text has (an unqualified name is in the schema the
 * table of the index is in, which for the table of the index that moved is the one it left).
 */
static bool
follow_ref(const SiftNameChange *c, Oid host, char **schema, char **name, char ***cols, int ncols, char **index_name)
{
	bool		changed = false;

	if (c->kind == SIFT_NAME_TABLE)
	{
		const char *now = sift_namespace_name(get_rel_namespace(host));
		const char *was = host == c->relid ? c->from_schema : now;

		if (strcmp(*schema ? *schema : was, c->from_schema) == 0 && strcmp(*name, c->from) == 0)
		{
			*name = pstrdup(c->to);
			if (*schema != NULL || strcmp(c->to_schema, now) != 0)
				*schema = pstrdup(c->to_schema);
			return true;
		}
		if (*schema == NULL && strcmp(was, now) != 0)
		{
			*schema = pstrdup(was);
			return true;
		}
		return false;
	}
	if (sift_conn_relation_oid(host, *schema, *name, NoLock, true) != c->relid)
		return false;
	if (c->kind == SIFT_NAME_INDEX && index_name != NULL && *index_name != NULL && strcmp(*index_name, c->from) == 0)
	{
		*index_name = pstrdup(c->to);
		return true;
	}
	for (int i = 0; c->kind == SIFT_NAME_COLUMN && i < ncols; i++)
		if (strcmp(*cols[i], c->from) == 0)
		{
			*cols[i] = pstrdup(c->to);
			changed = true;
		}
	return changed;
}

static void
put_ref(StringInfo s, const char *schema, const char *name, const char *a, const char *b)
{
	if (schema != NULL)
		appendStringInfo(s, "%s.", quote_identifier(schema));
	appendStringInfo(s, "%s(%s", quote_identifier(name), quote_identifier(a));
	if (b != NULL)
		appendStringInfo(s, ", %s", quote_identifier(b));
	appendStringInfoChar(s, ')');
}

static char *
connect_text(const char *text, Oid host, const SiftNameChange *c)
{
	const char *err = NULL;
	SiftConnSpecs *all = sift_conn_parse(text, &err);
	StringInfoData out;
	bool		changed = false;

	if (all == NULL)
		return NULL;
	initStringInfo(&out);
	if (all->automatic)
		appendStringInfoString(&out, "auto: ");
	for (int i = 0; i < all->n; i++)
	{
		SiftConnSpec *s = all->c[i];
		char	  **key[] = {&s->parent_key};

		changed |= follow_host(c, host, &s->fk);
		changed |= follow_ref(c, host, &s->parent_schema, &s->parent_table, key, 1, NULL);
		if (i > 0)
			appendStringInfoString(&out, "; ");
		appendStringInfo(&out, "%s -> ", quote_identifier(s->fk));
		put_ref(&out, s->parent_schema, s->parent_table, s->parent_key, NULL);
	}
	return changed ? out.data : NULL;
}

static bool
follow_scope_rel(const SiftNameChange *c, Oid host, SiftScopeRel *r)
{
	char	  **cols[] = {&r->a, &r->b};

	return follow_ref(c, host, &r->schema, &r->name, cols, 2, &r->index_name);
}

static void
put_scope_rel(StringInfo s, const SiftScopeRel *r)
{
	put_ref(s, r->schema, r->name, r->a, r->b);
	if (r->index_name != NULL)
		appendStringInfo(s, " using %s", quote_identifier(r->index_name));
}

static char *
scope_text(const char *text, Oid host, const SiftNameChange *c)
{
	const char *err = NULL;
	SiftScopeSpecs *all = sift_scope_parse(text, &err);
	StringInfoData out;
	bool		changed = false;

	if (all == NULL)
		return NULL;
	initStringInfo(&out);
	for (int i = 0; i < all->n; i++)
	{
		SiftScopeSpec *s = all->s[i];
		ListCell   *lc;
		char	   *sep = "";

		changed |= follow_host(c, host, &s->fk);
		changed |= follow_scope_rel(c, host, &s->rel);
		if (s->joined != NULL)
			changed |= follow_scope_rel(c, host, s->joined) | follow_scope_rel(c, host, s->via);
		foreach(lc, s->cols)
		{
			char	   *col = lfirst(lc);

			if (follow_host(c, host, &col))
			{
				lfirst(lc) = col;
				changed = true;
			}
		}
		if (i > 0)
			appendStringInfoString(&out, "; ");
		appendStringInfo(&out, "%s: %s -> ", quote_identifier(s->key_name), quote_identifier(s->fk));
		put_scope_rel(&out, &s->rel);
		if (s->joined != NULL)
		{
			appendStringInfoString(&out, " | ");
			put_scope_rel(&out, s->joined);
			appendStringInfoString(&out, " via ");
			put_scope_rel(&out, s->via);
		}
		if (s->cols != NIL)
		{
			appendStringInfoString(&out, " cols(");
			foreach_ptr(char, col, s->cols)
			{
				appendStringInfoString(&out, sep);
				appendStringInfoString(&out, quote_identifier(col));
				sep = ", ";
			}
			appendStringInfoChar(&out, ')');
		}
		if (s->max_multiplicity > 0)
			appendStringInfo(&out, " max_multiplicity(%.15g)", s->max_multiplicity);
	}
	return changed ? out.data : NULL;
}

List *
sift_names_renamed_options(Relation index, const SiftNameChange *c)
{
	Oid			host = index->rd_index->indrelid;
	HeapTuple	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(RelationGetRelid(index)));
	bool		isnull;
	Datum		d;
	List	   *out = NIL;

	if (!HeapTupleIsValid(tp))
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("relation with OID %u does not exist", RelationGetRelid(index))));
	d = SysCacheGetAttr(RELOID, tp, Anum_pg_class_reloptions, &isnull);
	if (!isnull)
		foreach_node(DefElem, opt, untransformRelOptions(d))
		{
			char	   *text = NULL;

			if (opt->arg == NULL)
				continue;
			if (strcmp(opt->defname, "connect") == 0)
				text = connect_text(defGetString(opt), host, c);
			else if (strcmp(opt->defname, "scope") == 0)
				text = scope_text(defGetString(opt), host, c);
			else if (c->kind == SIFT_NAME_COLUMN && host == c->relid)
				text = sift_names_list_option(opt->defname, defGetString(opt), c->from, c->to);
			if (text != NULL)
				out = lappend(out, makeDefElem(pstrdup(opt->defname), (Node *) makeString(text), -1));
		}
	ReleaseSysCache(tp);
	return out;
}
