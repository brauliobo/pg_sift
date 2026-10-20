/*
 * bm25/analyze_stem.c - stemmers. English is the Snowball English (Porter2)
 * algorithm as published in snowball/algorithms/english.sbl (Snowball 3),
 * implemented directly on ASCII bytes, plus the exception outputs (onli,
 * earli, ...) as invariant forms so that re-stemming keeps them. `make -C
 * tools/analyze check` validates it against the snowball-data English
 * vocabulary (42649 words).
 */
#include "core/host.h"

#include "bm25/analyze.h"

typedef struct Suffix
{
	const char *s;
	uint8		len;
	const char *to;				/* replacement; "*" or "" in step 2 = conditional */
	uint8		tolen;
} Suffix;

#define SFX(s, to) {s, sizeof(s) - 1, to, sizeof(to) - 1}
#define STR(s) {s, sizeof(s) - 1}

typedef struct Str
{
	const char *s;
	uint8		len;
} Str;

typedef struct Word
{
	char	   *w;
	int			len;
	int			p1;
	int			p2;
} Word;

static inline bool
is_v(char c)
{
	return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'y';
}

/* Whether a[0..n) == b[0..n); n is small, mismatches are usually early */
static inline bool
eq(const char *a, const char *b, int n)
{
	for (int i = 0; i < n; i++)
		if (a[i] != b[i])
			return false;
	return true;
}

static inline bool
ends(const Word *w, const char *s, int n)
{
	return w->len >= n && eq(w->w + w->len - n, s, n);
}

static inline bool
is_word(const Word *w, const char *s, int n)
{
	return w->len == n && eq(w->w, s, n);
}

/* Longest entry of tab (sorted by descending length) that w ends with */
static const Suffix *
longest(const Word *w, const Suffix *tab, int n)
{
	char		last = w->w[w->len - 1];

	for (int i = 0; i < n; i++)
		if (tab[i].s[tab[i].len - 1] == last && ends(w, tab[i].s, tab[i].len))
			return &tab[i];
	return NULL;
}

static inline void
replace(Word *w, int bra, const char *to, int n)
{
	memcpy(w->w + bra, to, n);
	w->len = bra + n;
}

#define REPLACE(w, bra, lit) replace(w, bra, lit, sizeof(lit) - 1)

/* Position after the first non-vowel that follows a vowel, from c; -1 if none */
static int
gopast_v_nonv(const Word *w, int c)
{
	while (c < w->len && !is_v(w->w[c]))
		c++;
	for (c++; c < w->len; c++)
		if (!is_v(w->w[c]))
			return c + 1;
	return -1;
}

static bool
has_vowel(const char *s, int n)
{
	for (int i = 0; i < n; i++)
		if (is_v(s[i]))
			return true;
	return false;
}

/* Snowball shortv, tested backwards at end */
static bool
shortv(const Word *w, int end)
{
	const char *s = w->w;
	char		c;

	if (end >= 3 && !is_v(c = s[end - 1]) && c != 'w' && c != 'x' && c != 'Y' &&
		is_v(s[end - 2]) && !is_v(s[end - 3]))
		return true;
	if (end == 2 && !is_v(s[1]) && is_v(s[0]))
		return true;
	return end >= 4 && eq(s + end - 4, "past", 4);
}

static bool
exception1(Word *w)
{
	static const Suffix tab[] = {
		SFX("skis", "ski"), SFX("skies", "sky"), SFX("idly", "idl"), SFX("gently", "gentl"),
		SFX("ugly", "ugli"), SFX("early", "earli"), SFX("only", "onli"), SFX("singly", "singl"),
		SFX("sky", "sky"), SFX("news", "news"), SFX("howe", "howe"), SFX("atlas", "atlas"),
		SFX("cosmos", "cosmos"), SFX("bias", "bias"), SFX("andes", "andes"),
		/* not in Snowball: the special forms above stay stems when stemmed again */
		SFX("ski", "ski"), SFX("idl", "idl"), SFX("gentl", "gentl"), SFX("ugli", "ugli"),
		SFX("earli", "earli"), SFX("onli", "onli"), SFX("singl", "singl"),
	};

	if (w->len > 6)
		return false;
	for (int i = 0; i < lengthof(tab); i++)
		if (is_word(w, tab[i].s, tab[i].len))
		{
			replace(w, 0, tab[i].to, tab[i].tolen);
			return true;
		}
	return false;
}

static bool
prelude(Word *w)
{
	bool		y_found = false;

	if (w->w[0] == '\'')
		memmove(w->w, w->w + 1, --w->len);
	for (int i = 0; i < w->len; i++)
		if (w->w[i] == 'y' && (i == 0 || is_v(w->w[i - 1])))
		{
			w->w[i] = 'Y';
			y_found = true;
		}
	return y_found;
}

static void
mark_regions(Word *w)
{
	static const Str pre[] = {STR("gener"), STR("commun"), STR("arsen"), STR("past"),
	STR("univers"), STR("later"), STR("emerg"), STR("organ"), STR("inter")};
	int			c = -1;

	w->p1 = w->p2 = w->len;
	for (int i = 0; i < lengthof(pre) && c < 0; i++)
		if (w->len >= pre[i].len && eq(w->w, pre[i].s, pre[i].len))
			c = pre[i].len;
	if (c < 0)
		c = gopast_v_nonv(w, 0);
	if (c < 0)
		return;
	w->p1 = c;
	c = gopast_v_nonv(w, c);
	if (c >= 0)
		w->p2 = c;
}

static void
step_1a(Word *w)
{
	int			bra;

	if (ends(w, "'s'", 3))
		w->len -= 3;
	else if (ends(w, "'s", 2))
		w->len -= 2;
	else if (ends(w, "'", 1))
		w->len -= 1;
	if (w->len == 0)
		return;
	if (ends(w, "sses", 4))
		w->len -= 2;
	else if (ends(w, "ied", 3) || ends(w, "ies", 3))
		replace(w, w->len - 3, "ie", w->len - 3 >= 2 ? 1 : 2);
	else if (ends(w, "us", 2) || ends(w, "ss", 2))
		return;
	else if (ends(w, "s", 1) && (bra = w->len - 1) >= 1 && has_vowel(w->w, bra - 1))
		w->len = bra;
}

/* Step 1b for the suffixes 'ed' 'edly' 'ing' 'ingly' starting at bra */
static void
step_1b_general(Word *w, int bra)
{
	static const char *const dbl[] = {"bb", "dd", "ff", "gg", "mm", "nn", "pp", "rr", "tt"};

	if (!has_vowel(w->w, bra))
		return;
	w->len = bra;
	if (ends(w, "at", 2) || ends(w, "bl", 2) || ends(w, "iz", 2))
	{
		w->w[w->len++] = 'e';
		return;
	}
	for (int i = 0; i < lengthof(dbl); i++)
		if (ends(w, dbl[i], 2))
		{
			if (!(w->len == 3 && (w->w[0] == 'a' || w->w[0] == 'e' || w->w[0] == 'o')))
				w->len--;
			return;
		}
	if (w->len == w->p1 && shortv(w, w->len))
		w->w[w->len++] = 'e';
}

static void
step_1b(Word *w)
{
	static const Str keep_ing[] = {STR("inn"), STR("out"), STR("cann"), STR("herr"), STR("earr"),
	STR("even")};
	int			n;

	if ((n = ends(w, "eedly", 5) ? 5 : ends(w, "eed", 3) ? 3 : 0))
	{
		int			bra = w->len - n;

		if (bra >= w->p1 &&
			!((bra == 4 && eq(w->w, "proc", 4)) || (bra == 3 && eq(w->w, "exc", 3)) ||
			  (bra == 4 && eq(w->w, "succ", 4))))
			REPLACE(w, bra, "ee");
		return;
	}
	if (ends(w, "ingly", 5) || ends(w, "edly", 4))
		return step_1b_general(w, w->len - (ends(w, "ingly", 5) ? 5 : 4));
	if (ends(w, "ing", 3))
	{
		int			bra = w->len - 3;

		if (bra == 2 && w->w[1] == 'y' && !is_v(w->w[0]))
			return REPLACE(w, 1, "ie");
		for (int i = 0; i < lengthof(keep_ing); i++)
			if (bra == keep_ing[i].len && eq(w->w, keep_ing[i].s, bra))
				return;
		return step_1b_general(w, bra);
	}
	if (ends(w, "ed", 2))
		step_1b_general(w, w->len - 2);
}

static void
step_1c(Word *w)
{
	int			n = w->len;

	if (n >= 3 && (w->w[n - 1] == 'y' || w->w[n - 1] == 'Y') && !is_v(w->w[n - 2]))
		w->w[n - 1] = 'i';
}

static void
step_2(Word *w)
{
	static const Suffix tab[] = {
		SFX("ization", "ize"), SFX("ational", "ate"), SFX("fulness", "ful"),
		SFX("ousness", "ous"), SFX("iveness", "ive"), SFX("tional", "tion"),
		SFX("biliti", "ble"), SFX("lessli", "less"), SFX("entli", "ent"), SFX("ation", "ate"),
		SFX("alism", "al"), SFX("aliti", "al"), SFX("ousli", "ous"), SFX("iviti", "ive"),
		SFX("fulli", "ful"), SFX("ogist", "og"), SFX("enci", "ence"), SFX("anci", "ance"),
		SFX("abli", "able"), SFX("izer", "ize"), SFX("ator", "ate"), SFX("alli", "al"),
		SFX("bli", "ble"), SFX("ogi", ""), SFX("li", ""),
	};
	const Suffix *m = longest(w, tab, lengthof(tab));
	int			bra;

	if (!m || (bra = w->len - m->len) < w->p1)
		return;
	if (m->tolen)
		replace(w, bra, m->to, m->tolen);
	else if (m->len == 3 && bra >= 1 && w->w[bra - 1] == 'l')
		REPLACE(w, bra, "og");
	else if (m->len == 2 && bra >= 1 && strchr("cdeghkmnrt", w->w[bra - 1]))
		w->len = bra;
}

static void
step_3(Word *w)
{
	static const Suffix tab[] = {
		SFX("ational", "ate"), SFX("tional", "tion"), SFX("alize", "al"), SFX("icate", "ic"),
		SFX("iciti", "ic"), SFX("ative", "*"), SFX("ical", "ic"), SFX("ness", ""), SFX("ful", ""),
	};
	const Suffix *m = longest(w, tab, lengthof(tab));
	int			bra;

	if (!m || (bra = w->len - m->len) < w->p1)
		return;
	if (m->to[0] != '*')
		replace(w, bra, m->to, m->tolen);
	else if (bra >= w->p2)
		w->len = bra;
}

static void
step_4(Word *w)
{
	static const Suffix tab[] = {
		SFX("ement", ""), SFX("ance", ""), SFX("ence", ""), SFX("able", ""), SFX("ible", ""),
		SFX("ment", ""), SFX("ant", ""), SFX("ent", ""), SFX("ism", ""), SFX("ate", ""),
		SFX("iti", ""), SFX("ous", ""), SFX("ive", ""), SFX("ize", ""), SFX("ion", "*"),
		SFX("al", ""), SFX("er", ""), SFX("ic", ""),
	};
	const Suffix *m = longest(w, tab, lengthof(tab));
	int			bra;

	if (!m || (bra = w->len - m->len) < w->p2)
		return;
	if (m->to[0] != '*' || (bra >= 1 && (w->w[bra - 1] == 's' || w->w[bra - 1] == 't')))
		w->len = bra;
}

static void
step_5(Word *w)
{
	int			bra = w->len - 1;
	char		c = w->w[bra];

	if (c == 'e' && (bra >= w->p2 || (bra >= w->p1 && !shortv(w, bra))))
		w->len = bra;
	else if (c == 'l' && bra >= w->p2 && bra >= 1 && w->w[bra - 1] == 'l')
		w->len = bra;
}

int
sift_analyze_stem_en(char *s, int len)
{
	Word		w = {s, len};
	bool		y_found;

	for (int i = 0; i < len; i++)
		if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '\''))
			return len;
	if (exception1(&w) || len < 3)
		return w.len;
	y_found = prelude(&w);
	mark_regions(&w);
	step_1a(&w);
	if (w.len > 0)
	{
		step_1b(&w);
		step_1c(&w);
		step_2(&w);
		step_3(&w);
		step_4(&w);
		step_5(&w);
	}
	if (y_found)
		for (int i = 0; i < w.len; i++)
			if (s[i] == 'Y')
				s[i] = 'y';
	return w.len;
}
